package com.orailnoor.droiddesk.runtime

import android.hardware.usb.UsbConfiguration
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.hardware.usb.UsbRequest
import android.net.LocalSocket
import android.util.Log
import java.io.DataInputStream
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * A USB Ethernet adapter taken from Android for Linux diagnostics (droiddesk-eth): raw frames
 * in both directions, promiscuous, while Android has no Ethernet.
 *
 * The adapter is driven as standard CDC-ECM. Realtek adapters (RTL8153, RTL8156) carry such a
 * configuration next to the vendor one the kernel's r8152 uses; switching to it detaches the
 * kernel driver, and switching back at the end lets it bind again, so Android's Ethernet
 * returns. Adapters with only a vendor protocol (ASIX AX88179) are not supported yet.
 *
 * Request "eth <name>" on the bridge socket; the reply is "ok <mac> ecm" or "err <reason>",
 * then frames length(2, big endian) + Ethernet frame (no FCS) in both directions until the
 * client closes the socket.
 */
object UsbEth {
    private const val TAG = "UsbEth"
    private const val CLASS_COMM = 0x02
    private const val SUBCLASS_ECM = 0x06
    private const val CLASS_CDC_DATA = 0x0A
    // SET_ETHERNET_PACKET_FILTER: promiscuous, all multicast, directed, broadcast, multicast.
    private const val PACKET_FILTER_ALL = 0x1F
    private const val RX_REQUESTS = 8
    private const val RX_BUFFER = 2048
    private const val MAX_FRAME = 1518

    private val open = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()

    private val nativeAvailable: Boolean = runCatching { System.loadLibrary("droiddesk_usb") }.isSuccess

    // usb_jni.c: Android's releaseInterface lets the kernel driver bind again at once, and
    // usbfs refuses a configuration change while any interface is bound.
    @JvmStatic private external fun nativeDriver(fd: Int, iface: Int, connect: Boolean): Int
    @JvmStatic private external fun nativeRelease(fd: Int, iface: Int): Int
    @JvmStatic private external fun nativeSetConfiguration(fd: Int, value: Int): Int

    /** Every interface number of the device, over all its configurations. */
    private fun interfaceNumbers(device: UsbDevice): Set<Int> =
        (0 until device.configurationCount).flatMap { c ->
            val config = device.getConfiguration(c)
            (0 until config.interfaceCount).map { config.getInterface(it).id }
        }.toSet()

    /** Detaches every kernel driver, then switches; false when usbfs refuses. */
    private fun switchConfiguration(connection: UsbDeviceConnection, device: UsbDevice, value: Int): Boolean {
        val fd = connection.fileDescriptor
        interfaceNumbers(device).forEach { nativeDriver(fd, it, false) }
        val result = nativeSetConfiguration(fd, value)
        if (result != 0) Log.w(TAG, "SETCONFIGURATION $value failed: errno ${-result}")
        return result == 0
    }

    private class Cdc(
        val config: UsbConfiguration,
        val comm: UsbInterface,
        val dataIdle: UsbInterface,
        val data: UsbInterface,
        val epIn: UsbEndpoint,
        val epOut: UsbEndpoint,
    )

    /** "ecm" when the adapter has a CDC-ECM configuration, "" otherwise (for "list"). */
    fun mode(device: UsbDevice): String = if (findCdc(device) != null) "ecm" else ""

    private fun findCdc(device: UsbDevice): Cdc? {
        for (c in 0 until device.configurationCount) {
            val config = device.getConfiguration(c)
            val interfaces = (0 until config.interfaceCount).map(config::getInterface)
            val comm = interfaces.firstOrNull { it.interfaceClass == CLASS_COMM && it.interfaceSubclass == SUBCLASS_ECM }
                ?: continue
            val dataAlts = interfaces.filter { it.interfaceClass == CLASS_CDC_DATA }
            val idle = dataAlts.firstOrNull { it.alternateSetting == 0 } ?: continue
            for (data in dataAlts.filter { it.id == idle.id }) {
                val endpoints = (0 until data.endpointCount).map(data::getEndpoint)
                    .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
                val epIn = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN } ?: continue
                val epOut = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT } ?: continue
                return Cdc(config, comm, idle, data, epIn, epOut)
            }
        }
        return null
    }

    /** Serves one "eth" client; the caller has checked the permission. */
    fun serve(usb: UsbManager, device: UsbDevice, client: LocalSocket) {
        val output = client.outputStream
        val cdc = findCdc(device) ?: run {
            output.write(("err %04x:%04x has no CDC-ECM configuration (only a vendor protocol, not supported yet)\n"
                .format(device.vendorId, device.productId)).toByteArray())
            return
        }
        if (!open.add(device.deviceName)) {
            output.write("err ${device.deviceName} is already taken over\n".toByteArray())
            return
        }
        val connection = usb.openDevice(device) ?: run {
            open.remove(device.deviceName)
            output.write("err Android could not open the device\n".toByteArray())
            return
        }
        val firstConfig = device.getConfiguration(0)
        var switched = false
        try {
            if (cdc.config.id != firstConfig.id) {
                // The kernel driver (r8152) holds the vendor configuration.
                if (!nativeAvailable || !switchConfiguration(connection, device, cdc.config.id)) {
                    output.write("err could not switch the adapter to CDC-ECM\n".toByteArray())
                    return
                }
                switched = true
            }
            if (!connection.claimInterface(cdc.comm, true) || !connection.claimInterface(cdc.dataIdle, true)) {
                output.write("err could not claim the adapter's interfaces\n".toByteArray())
                return
            }
            // Alternate setting 0 of the data interface has no endpoints; frames flow in 1.
            if (!connection.setInterface(cdc.data)) {
                output.write("err could not enable the data interface\n".toByteArray())
                return
            }
            connection.controlTransfer(0x21, 0x43, PACKET_FILTER_ALL, cdc.comm.id, null, 0, 1000)
            val mac = macAddress(connection, cdc) ?: "02:00:00:00:00:01"
            output.write("ok $mac ecm\n".toByteArray())
            output.flush()
            Log.i(TAG, "Took over ${device.deviceName} (%04x:%04x) as $mac".format(device.vendorId, device.productId))
            pump(connection, cdc, client)
        } catch (error: Exception) {
            Log.w(TAG, "Ethernet takeover ended", error)
            runCatching { output.write("err ${error.message}\n".toByteArray()) }
        } finally {
            if (switched) {
                // Back to the vendor configuration: the kernel probes its driver for it and
                // Android gets its Ethernet back. Otherwise unplugging the adapter does it.
                val fd = connection.fileDescriptor
                nativeRelease(fd, cdc.data.id)
                nativeRelease(fd, cdc.comm.id)
                if (!switchConfiguration(connection, device, firstConfig.id)) {
                    Log.w(TAG, "Could not give ${device.deviceName} back; unplug it to reset")
                }
            } else {
                runCatching { connection.releaseInterface(cdc.data) }
                runCatching { connection.releaseInterface(cdc.comm) }
            }
            connection.close()
            open.remove(device.deviceName)
            Log.i(TAG, "Gave ${device.deviceName} back to Android")
        }
    }

    /** Copies frames both ways until the client closes the socket or the adapter goes away. */
    private fun pump(connection: UsbDeviceConnection, cdc: Cdc, client: LocalSocket) {
        val output = client.outputStream
        val running = java.util.concurrent.atomic.AtomicBoolean(true)
        val receiver = thread(name = "usb-eth-rx", isDaemon = true) {
            val requests = (0 until RX_REQUESTS).map {
                UsbRequest().apply {
                    initialize(connection, cdc.epIn)
                    clientData = ByteBuffer.allocate(RX_BUFFER)
                }
            }
            try {
                requests.forEach { it.queue(it.clientData as ByteBuffer) }
                val header = ByteArray(2)
                while (running.get()) {
                    val done = try {
                        connection.requestWait(500)
                    } catch (timeout: java.util.concurrent.TimeoutException) {
                        continue
                    } ?: break
                    val buffer = done.clientData as ByteBuffer
                    val length = buffer.position()
                    if (length > 0) {
                        header[0] = (length shr 8).toByte()
                        header[1] = length.toByte()
                        synchronized(output) {
                            output.write(header)
                            output.write(buffer.array(), 0, length)
                            output.flush()
                        }
                    }
                    buffer.clear()
                    if (!done.queue(buffer)) break
                }
            } catch (error: Exception) {
                Log.d(TAG, "Receive ended: ${error.message}")
            } finally {
                running.set(false)
                requests.forEach { runCatching { it.cancel(); it.close() } }
                // Wakes the sending loop, which waits on the client.
                runCatching { client.shutdownInput() }
            }
        }
        val input = DataInputStream(client.inputStream)
        val frame = ByteArray(MAX_FRAME)
        val packet = cdc.epOut.maxPacketSize
        try {
            while (running.get()) {
                val length = input.readUnsignedShort()
                if (length > MAX_FRAME) error("frame of $length bytes")
                input.readFully(frame, 0, length)
                if (connection.bulkTransfer(cdc.epOut, frame, length, 1000) < 0) {
                    Log.d(TAG, "Dropped a frame of $length bytes")
                }
                // A frame filling whole packets needs a zero-length packet to end it.
                if (length % packet == 0) connection.bulkTransfer(cdc.epOut, frame, 0, 1000)
            }
        } catch (error: java.io.EOFException) {
            // The client is done.
        } catch (error: java.io.IOException) {
            // Socket closed (by the receiver when the adapter went away).
        } finally {
            running.set(false)
            receiver.join(2000)
        }
    }

    /** The MAC from the CDC Ethernet functional descriptor's iMACAddress string. */
    private fun macAddress(connection: UsbDeviceConnection, cdc: Cdc): String? {
        val raw = connection.rawDescriptors ?: return null
        var position = 0
        var configValue = -1
        while (position + 2 <= raw.size) {
            val length = raw[position].toInt() and 0xFF
            if (length < 2 || position + length > raw.size) break
            val type = raw[position + 1].toInt() and 0xFF
            if (type == 2 && length >= 6) configValue = raw[position + 5].toInt() and 0xFF
            if (type == 0x24 && length >= 4 && (raw[position + 2].toInt() and 0xFF) == 0x0F &&
                configValue == cdc.config.id
            ) {
                val index = raw[position + 3].toInt() and 0xFF
                val buffer = ByteArray(255)
                val read = connection.controlTransfer(0x80, 6, 0x0300 or index, 0x0409, buffer, buffer.size, 1000)
                if (read < 26) return null
                val hex = String(buffer, 2, read - 2, Charsets.UTF_16LE)
                if (!hex.matches(Regex("[0-9A-Fa-f]{12}"))) return null
                return hex.lowercase().chunked(2).joinToString(":")
            }
            position += length
        }
        return null
    }
}
