package com.orailnoor.droiddesk.runtime

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Hands USB devices to Linux programs. Without root, /dev/bus/usb is not
 * accessible; Android grants access per device and returns an open usbfs file
 * descriptor, which this bridge passes over a Unix socket (SCM_RIGHTS). The
 * Linux side wraps it with libusb_wrap_sys_device (droiddesk-usb in Debian).
 *
 * Protocol, one line per request on the abstract socket "droiddesk.usb":
 *   list              -> one tab-separated line per device, then an empty line
 *                        name, vid, pid, mass storage (1/0), manufacturer, product
 *   flash <name> <lun|-> <path>  (stable path, libaums like EtchDroid) writes a
 *                     raw, .xz or .gz image and verifies it by reading it back;
 *   read <name> <lun|-> <path>   copies the whole device into a file. "-" picks
 *                     the only inserted card of a multi-slot reader. Both stream
 *                     "progress <done> <total>" lines and end with
 *                     "done <sha256>" or "err <reason>"; closing the socket
 *                     cancels.
 *   open <name>       -> (raw, experimental) asks for permission if needed, claims the mass storage
 *                        interface and answers "ok <interface> <ep in> <ep out>"
 *                        with the fd attached, or "err <reason>". The device stays
 *                        open until the client closes the socket.
 */
object UsbBridge {
    private const val TAG = "UsbBridge"
    private const val SOCKET_NAME = "droiddesk.usb"
    private const val ACTION_PERMISSION = "com.orailnoor.droiddesk.USB_PERMISSION"
    private const val PERMISSION_TIMEOUT_S = 60L

    @Volatile private var server: LocalServerSocket? = null

    fun start(context: Context) {
        if (server != null) return
        synchronized(this) {
            if (server != null) return
            try {
                val socket = LocalServerSocket(SOCKET_NAME)
                server = socket
                thread(name = "usb-bridge", isDaemon = true) { serve(context.applicationContext, socket) }
                Log.i(TAG, "USB bridge started")
            } catch (error: Exception) {
                Log.e(TAG, "Could not start the USB bridge", error)
            }
        }
    }

    // Accepted clients, so stop() can end transfers and raw sessions too: once the
    // foreground service is gone nothing should keep holding a USB device.
    private val clients = java.util.concurrent.ConcurrentHashMap.newKeySet<LocalSocket>()

    fun stop() {
        synchronized(this) {
            val socket = server ?: return
            server = null
            runCatching { socket.close() }
        }
        clients.forEach { runCatching { it.shutdownInput(); it.close() } }
    }

    private fun serve(context: Context, socket: LocalServerSocket) {
        while (server === socket) {
            val client = try {
                socket.accept()
            } catch (error: Exception) {
                if (server === socket) Log.w(TAG, "USB bridge accept failed", error)
                continue
            }
            if (client.peerCredentials.uid != android.os.Process.myUid()) {
                Log.w(TAG, "Rejected USB request from uid ${client.peerCredentials.uid}")
                runCatching { client.close() }
                continue
            }
            // One thread per client: "open" blocks on the permission dialog and
            // then holds the device until the client disconnects.
            clients.add(client)
            thread(name = "usb-bridge-client", isDaemon = true) {
                try {
                    handle(context, client)
                } finally {
                    clients.remove(client)
                }
            }
        }
    }

    private fun handle(context: Context, client: LocalSocket) {
        try {
            val reader = client.inputStream.bufferedReader()
            val request = reader.readLine()?.trim().orEmpty()
            val output = client.outputStream
            val usb = context.getSystemService(UsbManager::class.java)
            when {
                request == "list" -> {
                    val lines = usb.deviceList.values.sortedBy { it.deviceName }.joinToString("") { device ->
                        listOf(
                            device.deviceName,
                            "%04x".format(device.vendorId),
                            "%04x".format(device.productId),
                            if (massStorageInterface(device) != null) "1" else "0",
                            clean(runCatching { device.manufacturerName }.getOrNull()),
                            clean(runCatching { device.productName }.getOrNull()),
                        ).joinToString("\t") + "\n"
                    }
                    output.write((lines + "\n").toByteArray())
                }
                request.startsWith("flash ") || request.startsWith("read ") -> {
                    // flash|read <device> <lun or -> <path>; the path may contain spaces.
                    val parts = request.split(' ', limit = 4)
                    val device = parts.getOrNull(1)?.let { usb.deviceList[it] }
                    val lun = parts.getOrNull(2)?.takeIf { it != "-" }?.toIntOrNull()
                    val path = parts.getOrNull(3)?.trim().orEmpty()
                    if (device == null || path.isEmpty() || (lun == null && parts.getOrNull(2) != "-")) {
                        output.write("err usage: flash|read <device> <lun|-> <path>\n".toByteArray())
                    } else if (!usb.hasPermission(device) && !requestPermission(context, usb, device)) {
                        output.write("err permission denied\n".toByteArray())
                    } else {
                        UsbFlasher(usb, device, output, lun, isCancelled = { server == null }) {
                            reader.readLine()?.trim() == "go"
                        }.run {
                            if (parts[0] == "flash") flash(java.io.File(path)) else readTo(java.io.File(path))
                        }
                    }
                }
                request.startsWith("open ") -> {
                    val name = request.removePrefix("open ").trim()
                    val device = usb.deviceList[name]
                    if (device == null) {
                        output.write("err no such device $name\n".toByteArray())
                        return
                    }
                    openAndHold(context, usb, device, client)
                }
                else -> output.write("err unknown request\n".toByteArray())
            }
            output.flush()
        } catch (error: Exception) {
            Log.w(TAG, "USB request failed", error)
        } finally {
            runCatching { client.close() }
        }
    }

    private fun openAndHold(context: Context, usb: UsbManager, device: UsbDevice, client: LocalSocket) {
        val output = client.outputStream
        if (!usb.hasPermission(device) && !requestPermission(context, usb, device)) {
            output.write("err permission denied\n".toByteArray())
            return
        }
        val iface = massStorageInterface(device)
        if (iface == null) {
            output.write("err not a USB mass storage device (BOT/SCSI)\n".toByteArray())
            return
        }
        val epIn = (0 until iface.endpointCount).map(iface::getEndpoint)
            .firstOrNull { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK && it.direction == UsbConstants.USB_DIR_IN }
        val epOut = (0 until iface.endpointCount).map(iface::getEndpoint)
            .firstOrNull { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK && it.direction == UsbConstants.USB_DIR_OUT }
        if (epIn == null || epOut == null) {
            output.write("err no bulk endpoints\n".toByteArray())
            return
        }
        val connection: UsbDeviceConnection = usb.openDevice(device) ?: run {
            output.write("err Android could not open the device\n".toByteArray())
            return
        }
        try {
            // force = true detaches the kernel's usb-storage driver; Android
            // unmounts the volume it may have mounted from it.
            if (!connection.claimInterface(iface, true)) {
                output.write("err could not claim the interface\n".toByteArray())
                return
            }
            ParcelFileDescriptor.fromFd(connection.fileDescriptor).use { fd ->
                client.setFileDescriptorsForSend(arrayOf(fd.fileDescriptor))
                output.write("ok ${iface.id} ${epIn.address} ${epOut.address}\n".toByteArray())
                output.flush()
            }
            client.setFileDescriptorsForSend(null)
            Log.i(TAG, "Handed ${device.deviceName} (%04x:%04x) to Linux".format(device.vendorId, device.productId))
            // Hold the connection until the Linux side is done.
            val input = client.inputStream
            while (input.read() >= 0) {
            }
            runCatching { connection.releaseInterface(iface) }
        } finally {
            connection.close()
            Log.i(TAG, "Released ${device.deviceName}")
        }
    }

    private fun requestPermission(context: Context, usb: UsbManager, device: UsbDevice): Boolean {
        val latch = CountDownLatch(1)
        var granted = false
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                latch.countDown()
            }
        }
        ContextCompat.registerReceiver(
            context,
            receiver,
            IntentFilter(ACTION_PERMISSION),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        try {
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val intent = PendingIntent.getBroadcast(
                context,
                device.deviceId,
                Intent(ACTION_PERMISSION).setPackage(context.packageName),
                flags,
            )
            usb.requestPermission(device, intent)
            latch.await(PERMISSION_TIMEOUT_S, TimeUnit.SECONDS)
        } finally {
            runCatching { context.unregisterReceiver(receiver) }
        }
        return granted && usb.hasPermission(device)
    }

    /** First interface that speaks USB mass storage Bulk-Only Transport with SCSI. */
    private fun massStorageInterface(device: UsbDevice): UsbInterface? =
        (0 until device.interfaceCount).map(device::getInterface).firstOrNull {
            it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                it.interfaceSubclass == 0x06 && it.interfaceProtocol == 0x50
        }

    private fun clean(value: String?): String = value.orEmpty().replace(Regex("[\\t\\n\\r]"), " ").trim()
}
