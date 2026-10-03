package com.orailnoor.droiddesk.runtime

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.LocalSocket
import android.util.Log
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.io.DataInputStream
import java.io.OutputStream
import kotlin.concurrent.thread

/**
 * USB serial adapters (FTDI, CP210x, CH340/CH341/CH343/CH9102, PL2303, CDC-ACM) for Linux
 * programs, through usb-serial-for-android: Linux has no driver for them without root.
 *
 * Request "serial <name> <port>" on the bridge socket; the reply is "ok <driver> <ports>" or
 * "err <reason>", then frames type(1) length(2, big endian) payload in both directions:
 *   to the adapter:  D data, P baud(4) data bits(1) stop bits(1: 1, 2, 3 = 1.5) parity(1: 0 none,
 *                    1 odd, 2 even, 3 mark, 4 space), M dtr(1) rts(1), B break milliseconds(2)
 *   from it:         D data, S modem lines(1: CTS 1, DSR 2, CD 4, RI 8), E error text (the
 *                    socket closes after a fatal one, e.g. unplugged)
 * The port stays open until the client closes the socket (droiddesk-serial, the pty side).
 */
object UsbSerial {
    private const val TAG = "UsbSerial"
    private const val WRITE_TIMEOUT_MS = 2000
    private const val LINES_POLL_MS = 200L

    private val open = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()

    /** Number of serial ports of a supported adapter, 0 for anything else. */
    fun ports(device: UsbDevice): Int =
        runCatching { UsbSerialProber.getDefaultProber().probeDevice(device)?.ports?.size ?: 0 }.getOrDefault(0)

    /** Serves one "serial" client; the caller has checked the permission. */
    fun serve(usb: UsbManager, device: UsbDevice, index: Int, client: LocalSocket) {
        val output = client.outputStream
        val driver = UsbSerialProber.getDefaultProber().probeDevice(device)
        val port = driver?.ports?.getOrNull(index)
        if (port == null) {
            output.write("err not a supported serial adapter or no port $index\n".toByteArray())
            return
        }
        val key = "${device.deviceName}:$index"
        if (!open.add(key)) {
            output.write("err port $index of ${device.deviceName} is already open\n".toByteArray())
            return
        }
        try {
            val connection = usb.openDevice(device)
            if (connection == null) {
                output.write("err Android could not open the device\n".toByteArray())
                return
            }
            try {
                port.open(connection)
            } catch (error: Exception) {
                connection.close()
                output.write("err could not open the port: ${clean(error.message)}\n".toByteArray())
                return
            }
            // Closed in every case from here on (also a client gone before the reply);
            // closing the port closes the connection too.
            try {
                val name = driver.javaClass.simpleName.removeSuffix("SerialDriver").removeSuffix("Driver")
                output.write("ok $name ${driver.ports.size}\n".toByteArray())
                output.flush()
                Log.i(TAG, "Serial port $key ($name) open for Linux")
                runSession(port, client, output)
            } finally {
                runCatching { port.close() }
            }
        } finally {
            open.remove(key)
            Log.i(TAG, "Serial port $key closed")
        }
    }

    private fun runSession(port: UsbSerialPort, client: LocalSocket, output: OutputStream) {
        val lock = Any()
        val alive = java.util.concurrent.atomic.AtomicBoolean(true)
        fun send(type: Char, payload: ByteArray) {
            synchronized(lock) {
                var at = 0
                do {
                    val n = minOf(payload.size - at, 0xFFFF)
                    output.write(byteArrayOf(type.code.toByte(), (n shr 8).toByte(), n.toByte()))
                    output.write(payload, at, n)
                    at += n
                } while (at < payload.size)
                output.flush()
            }
        }

        val io = SerialInputOutputManager(port, object : SerialInputOutputManager.Listener {
            override fun onNewData(data: ByteArray) {
                runCatching { send('D', data) }.onFailure { alive.set(false) }
            }

            override fun onRunError(error: Exception) {
                if (alive.get()) {
                    // Unplugged, or the driver gave up: tell the pty side and end.
                    runCatching { send('E', clean(error.message).ifEmpty { "adapter error" }.toByteArray()) }
                    alive.set(false)
                    runCatching { client.shutdownInput() }
                }
            }
        })
        io.start()

        // Modem status lines: not every chip reports them (CDC-ACM has none to poll).
        val supported = runCatching { port.supportedControlLines }.getOrNull().orEmpty()
        val polled = supported.intersect(setOf(UsbSerialPort.ControlLine.CTS, UsbSerialPort.ControlLine.DSR,
            UsbSerialPort.ControlLine.CD, UsbSerialPort.ControlLine.RI))
        val poller = if (polled.isEmpty()) null else thread(name = "usb-serial-lines", isDaemon = true) {
            var last = -1
            while (alive.get()) {
                val lines = runCatching { port.controlLines }.getOrNull()
                if (lines != null) {
                    var bits = 0
                    if (UsbSerialPort.ControlLine.CTS in lines) bits = bits or 1
                    if (UsbSerialPort.ControlLine.DSR in lines) bits = bits or 2
                    if (UsbSerialPort.ControlLine.CD in lines) bits = bits or 4
                    if (UsbSerialPort.ControlLine.RI in lines) bits = bits or 8
                    if (bits != last) {
                        last = bits
                        runCatching { send('S', byteArrayOf(bits.toByte())) }.onFailure { alive.set(false) }
                    }
                }
                Thread.sleep(LINES_POLL_MS)
            }
        }

        try {
            val input = DataInputStream(client.inputStream.buffered())
            while (alive.get()) {
                val type = input.read()
                if (type < 0) break
                val length = input.readUnsignedShort()
                val payload = ByteArray(length)
                input.readFully(payload)
                try {
                    when (type.toChar()) {
                        'D' -> port.write(payload, WRITE_TIMEOUT_MS)
                        'P' -> if (length >= 7) {
                            val baud = ((payload[0].toInt() and 0xFF) shl 24) or ((payload[1].toInt() and 0xFF) shl 16) or
                                ((payload[2].toInt() and 0xFF) shl 8) or (payload[3].toInt() and 0xFF)
                            port.setParameters(baud, payload[4].toInt(), payload[5].toInt(), payload[6].toInt())
                        }
                        'M' -> if (length >= 2) {
                            if (UsbSerialPort.ControlLine.DTR in supported) port.dtr = payload[0].toInt() != 0
                            if (UsbSerialPort.ControlLine.RTS in supported) port.rts = payload[1].toInt() != 0
                        }
                        'B' -> if (length >= 2) {
                            val ms = ((payload[0].toInt() and 0xFF) shl 8) or (payload[1].toInt() and 0xFF)
                            port.setBreak(true)
                            Thread.sleep(ms.coerceIn(1, 5000).toLong())
                            port.setBreak(false)
                        }
                    }
                } catch (error: UnsupportedOperationException) {
                    // A chip without break or this format: the program goes on, the user sees why.
                    send('E', ("unsupported: " + clean(error.message)).toByteArray())
                } catch (error: IllegalArgumentException) {
                    send('E', ("unsupported: " + clean(error.message)).toByteArray())
                }
            }
        } catch (error: Exception) {
            if (alive.get()) Log.w(TAG, "Serial session ended", error)
        } finally {
            alive.set(false)
            io.stop()
            poller?.interrupt()
        }
    }

    private fun clean(value: String?): String = value.orEmpty().replace(Regex("[\\t\\n\\r]"), " ").trim()
}
