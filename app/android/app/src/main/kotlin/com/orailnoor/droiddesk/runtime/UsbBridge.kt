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
 *                        name, vid, pid, mass storage (1/0), manufacturer, product, held (1/0),
 *                        serial ports (0 = not a supported USB serial adapter)
 *   flash <name> <lun|-> <path>  (stable path, libaums like EtchDroid) writes a
 *                     raw, .xz or .gz image and verifies it by reading it back;
 *   read <name> <lun|-> <path>   copies the whole device into a file. "-" picks
 *                     the only inserted card of a multi-slot reader. Both stream
 *                     "progress <done> <total>" lines and end with
 *                     "done <sha256>" or "err <reason>"; closing the socket
 *                     cancels.
 *   blk <name> <lun|-> -> "device <capacity> <block size> <slot> <slots>", then binary
 *                     block requests (see UsbFlasher.serveBlocks) for libdroiddesk_blk.so
 *                     and droiddesk-usb format/files; holds the device lock until the end.
 *   attach <name>     -> asks for permission and holds the device for Linux ("ok"/"err …")
 *   eject <name>      -> hands a held device back to Android
 *   watch             -> stays open and streams "attached\t<list line>", "detached\t<name>",
 *                        "held\t<name>" and "released\t<name>"
 *   serial <name> <port> -> a USB serial adapter's port as a framed byte stream (UsbSerial)
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
                watchUsbEvents(context.applicationContext)
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
        held.keys.toList().forEach(::eject)
        appContext?.let { context -> usbEvents?.let { runCatching { context.unregisterReceiver(it) } } }
        usbEvents = null
    }

    // ── Events for the "USB disky" panel icon (request "watch") ──

    private val watchers = java.util.concurrent.CopyOnWriteArrayList<java.io.OutputStream>()
    @Volatile private var usbEvents: BroadcastReceiver? = null
    @Volatile private var appContext: Context? = null

    private fun watchUsbEvents(context: Context) {
        appContext = context
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                @Suppress("DEPRECATION")
                val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
                when (intent.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> emit("attached\t" + describe(device))
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                        if (held.containsKey(device.deviceName)) eject(device.deviceName)
                        // Unplugged: the device's cache is gone with its power.
                        val gone = shapeId(device)
                        readShapes.keys.removeAll { it.startsWith("$gone:") }
                        shapeDir(context).listFiles()?.filter { it.name.startsWith("$gone:") }?.forEach { it.delete() }
                        emit("detached\t${device.deviceName}")
                    }
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        // System broadcasts still reach a not-exported receiver.
        ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
        usbEvents = receiver
    }

    private fun emit(line: String) {
        val bytes = "$line\n".toByteArray()
        watchers.forEach { out ->
            runCatching { synchronized(out) { out.write(bytes); out.flush() } }.onFailure { watchers.remove(out) }
        }
    }

    /** The tab-separated device line of "list" (name, vid, pid, storage, maker, product, held, serial ports). */
    private fun describe(device: UsbDevice): String = listOf(
        device.deviceName,
        "%04x".format(device.vendorId),
        "%04x".format(device.productId),
        if (massStorageInterface(device) != null) "1" else "0",
        clean(runCatching { device.manufacturerName }.getOrNull()),
        clean(runCatching { device.productName }.getOrNull()),
        if (held.containsKey(device.deviceName)) "1" else "0",
        UsbSerial.ports(device).toString(),
    ).joinToString("\t")

    /**
     * A mass storage device DroidDesk keeps claimed between operations. Handing
     * the reader back to Android after every read or flash made the kernel and
     * vold rescan it (sgdisk) while the next operation already claimed it again,
     * and readers behind the DeX dock got stuck. The device stays with Linux until
     * "eject", the end of the session, or unplugging.
     */
    class HeldDevice(
        val device: UsbDevice,
        val connection: UsbDeviceConnection,
        val iface: UsbInterface,
        val epIn: android.hardware.usb.UsbEndpoint,
        val epOut: android.hardware.usb.UsbEndpoint,
    )

    private val held = java.util.concurrent.ConcurrentHashMap<String, HeldDevice>()

    /** READ shapes per "device name:slot", kept while plugged in (see ReadShapes). */
    private val readShapes = java.util.concurrent.ConcurrentHashMap<String, ReadShapes>()

    fun readShapesFor(device: UsbDevice, slot: Int, step: Int): ReadShapes {
        val key = "${shapeId(device)}:$slot:$step"
        return readShapes.getOrPut(key) {
            appContext?.let { ReadShapes.load(java.io.File(shapeDir(it), key), step) } ?: ReadShapes(step)
        }
    }

    /** Saves the read shapes of a device after a session (they outlive a DroidDesk restart). */
    fun saveReadShapes(device: UsbDevice) {
        val context = appContext ?: return
        val id = shapeId(device)
        for ((key, shapes) in readShapes) {
            if (key.startsWith("$id:") && shapes.dirty) {
                runCatching { shapes.save(java.io.File(shapeDir(context), key)) }
                    .onFailure { Log.w(TAG, "Could not save read shapes of $key", it) }
            }
        }
    }

    private fun shapeDir(context: Context) = java.io.File(context.filesDir, "usb-read-shapes").apply { mkdirs() }

    /** Stable across reconnects of the same device: vendor, product and serial number. */
    private fun shapeId(device: UsbDevice): String {
        val serial = runCatching { device.serialNumber }.getOrNull()?.filter { it.isLetterOrDigit() }.orEmpty()
        return "%04x-%04x-%s".format(device.vendorId, device.productId, serial.ifEmpty { "noserial" })
    }

    /** The held connection for [device], claiming it first if needed; an error text otherwise. */
    @Synchronized
    private fun hold(usb: UsbManager, device: UsbDevice): Any {
        held[device.deviceName]?.let { existing ->
            if (existing.device.deviceId == device.deviceId) return existing
            eject(device.deviceName) // unplugged and replugged under the same name
        }
        val iface = massStorageInterface(device) ?: return "not a USB mass storage device (BOT/SCSI)"
        val endpoints = (0 until iface.endpointCount).map(iface::getEndpoint)
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val epIn = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN } ?: return "no bulk IN endpoint"
        val epOut = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT } ?: return "no bulk OUT endpoint"
        val connection = usb.openDevice(device) ?: return "Android could not open the device"
        // force = true detaches the kernel's usb-storage driver; Android unmounts the card.
        if (!connection.claimInterface(iface, true)) {
            connection.close()
            return "could not claim the interface"
        }
        return HeldDevice(device, connection, iface, epIn, epOut).also {
            held[device.deviceName] = it
            Log.i(TAG, "Holding ${device.deviceName} for Linux until eject")
            emit("held\t${device.deviceName}")
        }
    }

    /** Gives the device back to Android (its usb-storage driver and vold). */
    fun eject(name: String): Boolean {
        val device = held.remove(name) ?: return false
        synchronized(device) {
            runCatching { device.connection.releaseInterface(device.iface) }
            device.connection.close()
        }
        Log.i(TAG, "Released $name to Android")
        emit("released\t$name")
        return true
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
                    // Forget held devices that were unplugged.
                    held.keys.filter { it !in usb.deviceList }.forEach(::eject)
                    val lines = usb.deviceList.values.sortedBy { it.deviceName }
                        .joinToString("") { describe(it) + "\n" }
                    output.write((lines + "\n").toByteArray())
                }
                request.startsWith("flash ") || request.startsWith("read ") || request.startsWith("blk ") -> {
                    // flash|read <device> <lun or -> <path>; the path may contain spaces.
                    // blk <device> <lun or -> has no path.
                    val parts = request.split(' ', limit = 4)
                    val device = parts.getOrNull(1)?.let { usb.deviceList[it] }
                    val lun = parts.getOrNull(2)?.takeIf { it != "-" }?.toIntOrNull()
                    val path = parts.getOrNull(3)?.trim().orEmpty()
                    val blk = parts[0] == "blk"
                    if (device == null || (path.isEmpty() && !blk) || (lun == null && parts.getOrNull(2) != "-")) {
                        output.write("err usage: flash|read <device> <lun|-> <path>, blk <device> <lun|->\n".toByteArray())
                    } else if (!usb.hasPermission(device) && !run {
                            // The dialog appears on the phone, which may not be the screen in
                            // use (DeX); the client tells the user where to look.
                            output.write("permission\n".toByteArray())
                            output.flush()
                            requestPermission(context, usb, device)
                        }
                    ) {
                        output.write("err permission denied\n".toByteArray())
                    } else {
                        when (val holding = hold(usb, device)) {
                            is HeldDevice -> UsbFlasher(holding, output, lun, isCancelled = { server == null }) {
                                blk || reader.readLine()?.trim() == "go"
                            }.run {
                                when (parts[0]) {
                                    "flash" -> flash(java.io.File(path))
                                    "read" -> readTo(java.io.File(path))
                                    // The client sends nothing after the request line before the
                                    // device line, so the reader has buffered nothing extra.
                                    else -> serveBlocks(client.inputStream)
                                }
                            }
                            else -> output.write("err $holding\n".toByteArray())
                        }
                    }
                }
                request == "watch" -> {
                    // Stays open; events are written by emit() until the client goes away.
                    watchers.add(output)
                    try {
                        while (client.inputStream.read() >= 0) {
                        }
                    } finally {
                        watchers.remove(output)
                    }
                }
                request.startsWith("attach ") -> {
                    // Hold without an operation ("Připojit do Linuxu" in the panel icon).
                    val name = request.removePrefix("attach ").trim()
                    val device = usb.deviceList[name]
                    val result = when {
                        device == null -> "err no such device $name"
                        !usb.hasPermission(device) && !run {
                            output.write("permission\n".toByteArray())
                            output.flush()
                            requestPermission(context, usb, device)
                        } -> "err permission denied"
                        else -> when (val holding = hold(usb, device)) {
                            is HeldDevice -> "ok"
                            else -> "err $holding"
                        }
                    }
                    output.write("$result\n".toByteArray())
                }
                request.startsWith("eject ") -> {
                    val name = request.removePrefix("eject ").trim()
                    output.write((if (eject(name)) "ok\n" else "err $name is not held\n").toByteArray())
                }
                request.startsWith("serial ") -> {
                    val parts = request.split(' ')
                    val device = parts.getOrNull(1)?.let { usb.deviceList[it] }
                    val index = parts.getOrNull(2)?.toIntOrNull() ?: 0
                    when {
                        device == null -> output.write("err no such device ${parts.getOrNull(1).orEmpty()}\n".toByteArray())
                        held.containsKey(device.deviceName) ->
                            output.write("err ${device.deviceName} is held as a disk\n".toByteArray())
                        !usb.hasPermission(device) && !run {
                            output.write("permission\n".toByteArray())
                            output.flush()
                            requestPermission(context, usb, device)
                        } -> output.write("err permission denied\n".toByteArray())
                        else -> UsbSerial.serve(usb, device, index, client)
                    }
                }
                request.startsWith("open ") -> {
                    val name = request.removePrefix("open ").trim()
                    val device = usb.deviceList[name]
                    if (device == null) {
                        output.write("err no such device $name\n".toByteArray())
                        return
                    }
                    if (held.containsKey(name)) {
                        output.write("err $name is held for read/flash; run droiddesk-usb eject first\n".toByteArray())
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
                // All requests share the action; only this device's answer counts.
                @Suppress("DEPRECATION")
                val answered = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                if (answered?.deviceName != device.deviceName) return
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
            // On the Fold 7 the result broadcast did not always arrive although
            // the grant was stored, so the request timed out after OK. Check the
            // permission itself while waiting as well.
            val deadline = android.os.SystemClock.elapsedRealtime() + PERMISSION_TIMEOUT_S * 1000
            while (!latch.await(500, TimeUnit.MILLISECONDS)) {
                if (usb.hasPermission(device)) return true
                if (android.os.SystemClock.elapsedRealtime() > deadline) break
            }
        } finally {
            runCatching { context.unregisterReceiver(receiver) }
        }
        return usb.hasPermission(device)
    }

    /** First interface that speaks USB mass storage Bulk-Only Transport with SCSI. */
    private fun massStorageInterface(device: UsbDevice): UsbInterface? =
        (0 until device.interfaceCount).map(device::getInterface).firstOrNull {
            it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                it.interfaceSubclass == 0x06 && it.interfaceProtocol == 0x50
        }

    private fun clean(value: String?): String = value.orEmpty().replace(Regex("[\\t\\n\\r]"), " ").trim()
}
