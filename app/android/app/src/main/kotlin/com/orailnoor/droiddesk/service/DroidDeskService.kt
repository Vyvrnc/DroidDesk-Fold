package com.orailnoor.droiddesk.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.os.BatteryManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.orailnoor.droiddesk.MainActivity
import com.orailnoor.droiddesk.runtime.AndroidAppBridge
import com.orailnoor.droiddesk.runtime.LinuxRuntime
import com.orailnoor.droiddesk.runtime.NetBridge
import com.orailnoor.droiddesk.runtime.UsbBridge
import com.orailnoor.droiddesk.x11.X11ServerService
import java.io.File

/**
 * Foreground service that keeps the Linux runtime alive.
 *
 * Android aggressively kills background processes (especially Android 12+'s
 * Phantom Process Killer). This service ensures our native Termux/chroot session, desktop
 * environment, and Wayland compositor survive when the user switches apps.
 */
class DroidDeskService : Service() {

    companion object {
        const val CHANNEL_ID = "droiddesk_service"
        const val NOTIFICATION_ID = 1001

        // The last network seen, so a Debian installed later on an unchanged
        // network still gets Android's DNS (see writeDebianResolvConf).
        @Volatile private var lastLinkProperties: LinkProperties? = null

        /** True between onCreate and onDestroy: Linux runs, with or without the desktop. */
        @Volatile var running = false
            private set

        /** Rewrites the Debian resolv.conf from the last known network, if any. */
        fun applyDebianDns(context: Context) {
            // Debian can be installed before the service ever ran; ask Android directly then.
            val linkProperties = lastLinkProperties ?: runCatching {
                val connectivity = context.getSystemService(ConnectivityManager::class.java)
                connectivity.getLinkProperties(connectivity.activeNetwork)
            }.getOrNull()
            linkProperties?.let { writeDebianResolvConf(context, it) }
        }

        private fun writeDebianResolvConf(context: Context, linkProperties: LinkProperties) {
            val servers = linkProperties.dnsServers
                .filterNot { it.isLinkLocalAddress }
                .mapNotNull { it.hostAddress }
                .take(3)
            if (servers.isEmpty()) return
            val etc = File(LinuxRuntime.debianRootfs(context.filesDir), "etc")
            if (!etc.isDirectory) return
            try {
                val resolvConf = File(etc, "resolv.conf")
                // A symlink would point at a path that only exists inside a real system.
                if (java.nio.file.Files.isSymbolicLink(resolvConf.toPath())) resolvConf.delete()
                resolvConf.writeText(
                    buildString {
                        append("# Written by DroidDesk from Android's active network\n")
                        servers.forEach { append("nameserver ").append(it).append('\n') }
                        linkProperties.domains?.takeIf { it.isNotBlank() }?.let {
                            append("search ").append(it.replace(',', ' ')).append('\n')
                        }
                    },
                )
            } catch (_: Exception) {
            }
        }
    }

    private var wakeLock: PowerManager.WakeLock? = null

    // The X server runs in its own :x11 process. Its only binding came from
    // DesktopActivity, so once the activity went away the process sank to a
    // plain started service and lmkd killed it first (LOW_MEMORY in exit-info),
    // taking the whole XFCE session with it. Binding it from this foreground
    // service lends it the same priority for as long as the session runs.
    private val x11Connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, service: IBinder) {}
        override fun onServiceDisconnected(name: ComponentName) {}
    }
    private var x11Bound = false

    // XFCE has no battery source here: xfce4-power-manager waits forever on
    // termux-api, which needs the Termux:API app. Publish the level to a file
    // that the dock's genmon plugin reads (droiddesk-battery).
    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
            if (level < 0 || scale <= 0) return
            val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
            val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
            try {
                File(filesDir, "tmp").mkdirs()
                File(filesDir, "tmp/droiddesk-battery")
                    .writeText("${level * 100 / scale} ${if (charging) 1 else 0}\n")
            } catch (_: Exception) {
            }
        }
    }

    // Debian's glibc resolver reads /etc/resolv.conf, which proot-distro fills
    // with 8.8.8.8, so it ignored Android's VPN (e.g. a company VPN) and the
    // network's own DNS. Mirror the DNS servers of the default network, which
    // is the VPN while one is active. Termux's bionic programs ask Android
    // directly and need nothing.
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {
            lastLinkProperties = linkProperties
            writeDebianResolvConf(this@DroidDeskService, linkProperties)
        }
    }

    override fun onCreate() {
        super.onCreate()
        AndroidAppBridge.start(this)
        UsbBridge.start(this)
        NetBridge.start(this)
        running = true
        createNotificationChannel()
        acquireWakeLock()
        x11Bound = bindService(
            Intent(this, X11ServerService::class.java),
            x11Connection,
            Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT,
        )
        registerReceiver(batteryReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        runCatching {
            getSystemService(ConnectivityManager::class.java).registerDefaultNetworkCallback(networkCallback)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification("Linux desktop is running")

        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            notification,
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            } else {
                0
            }
        )

        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        running = false
        unregisterReceiver(batteryReceiver)
        runCatching {
            getSystemService(ConnectivityManager::class.java).unregisterNetworkCallback(networkCallback)
        }
        if (x11Bound) {
            unbindService(x11Connection)
            x11Bound = false
        }
        AndroidAppBridge.stop()
        UsbBridge.stop()
        NetBridge.stop()
        releaseWakeLock()
        super.onDestroy()
    }

    // ── Notification ──

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "DroidDesk Linux Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Keeps the Linux desktop environment running"
                setShowBadge(false)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(contentText: String): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("DroidDesk")
            .setContentText(contentText)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    fun updateNotification(text: String) {
        val notification = buildNotification(text)
        val manager = getSystemService(NotificationManager::class.java)
        manager.notify(NOTIFICATION_ID, notification)
    }

    // ── Wake Lock ──

    private fun acquireWakeLock() {
        val powerManager = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "DroidDesk::LinuxRuntime"
        ).apply {
            acquire(Long.MAX_VALUE)  // Keep CPU alive
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let {
            if (it.isHeld) it.release()
        }
        wakeLock = null
    }
}
