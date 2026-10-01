package com.orailnoor.droiddesk.runtime

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.util.Log
import me.jahnen.libaums.core.driver.BlockDeviceDriver
import me.jahnen.libaums.core.driver.BlockDeviceDriverFactory
import me.jahnen.libaums.core.driver.scsi.commands.sense.MediaNotInserted
import me.jahnen.libaums.core.usb.UsbCommunication
import me.jahnen.libaums.core.usb.UsbCommunicationFactory
import org.tukaani.xz.SeekableFileInputStream
import org.tukaani.xz.SeekableXZInputStream
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.zip.GZIPInputStream

/**
 * Stable flashing path of droiddesk-usb: SCSI over Bulk-Only Transport through
 * libaums, the way EtchDroid (GPL-3.0) does it. Raw images only — the image is
 * copied block for block, nothing is interpreted (a Windows ISO needs to be
 * turned into a disk image first).
 */
class UsbFlasher(
    private val usb: UsbManager,
    private val device: UsbDevice,
    private val output: OutputStream,
    /** Called after the device line; returns false to stop before any write. */
    private val confirm: () -> Boolean,
) {
    private companion object {
        const val TAG = "UsbFlasher"
        const val CHUNK = 1 shl 20
        const val PROGRESS_EVERY_MS = 500L
    }

    private var lastProgress = 0L

    fun flash(image: File) = withBlockDevice { block ->
        if (!image.isFile) return@withBlockDevice fail("no such image ${image.path}")
        val capacity = block.blocks * block.blockSize
        val total = uncompressedSize(image)
        if (total > capacity) {
            return@withBlockDevice fail("image is $total bytes, the device only $capacity")
        }
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteBuffer.allocate(CHUNK)
        var written = 0L
        var lba = 0L
        openImage(image).use { input ->
            while (true) {
                val read = fill(input, buffer.array())
                if (read <= 0) break
                if (written + read > capacity) return@withBlockDevice fail("image is larger than the device ($capacity bytes)")
                digest.update(buffer.array(), 0, read)
                // The device takes whole blocks; pad the last one with zeros.
                val padded = (read + block.blockSize - 1) / block.blockSize * block.blockSize
                java.util.Arrays.fill(buffer.array(), read, padded, 0)
                buffer.clear().limit(padded)
                block.write(lba, buffer)
                lba += padded / block.blockSize
                written += read
                progress(written, total)
            }
        }
        val expected = hex(digest.digest())
        line("verify")
        val check = MessageDigest.getInstance("SHA-256")
        var verified = 0L
        lba = 0L
        while (verified < written) {
            val want = minOf(CHUNK.toLong(), written - verified).toInt()
            val padded = (want + block.blockSize - 1) / block.blockSize * block.blockSize
            buffer.clear().limit(padded)
            block.read(lba, buffer)
            check.update(buffer.array(), 0, want)
            lba += padded / block.blockSize
            verified += want
            progress(verified, written)
        }
        val actual = hex(check.digest())
        if (actual != expected) return@withBlockDevice fail("verify failed: wrote $expected, read back $actual")
        line("done $expected")
    }

    fun readTo(target: File) = withBlockDevice { block ->
        val total = block.blocks * block.blockSize
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteBuffer.allocate(CHUNK - CHUNK % block.blockSize)
        var done = 0L
        var lba = 0L
        target.outputStream().buffered(CHUNK).use { out ->
            while (done < total) {
                val want = minOf(buffer.capacity().toLong(), total - done).toInt()
                buffer.clear().limit(want)
                block.read(lba, buffer)
                out.write(buffer.array(), 0, want)
                digest.update(buffer.array(), 0, want)
                lba += want / block.blockSize
                done += want
                progress(done, total)
            }
        }
        line("done ${hex(digest.digest())}")
    }

    private fun withBlockDevice(action: (BlockDeviceDriver) -> Unit) {
        val iface = massStorageInterface() ?: return fail("not a USB mass storage device (BOT/SCSI)")
        val endpoints = (0 until iface.endpointCount).map(iface::getEndpoint)
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val epIn = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN }
        val epOut = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT }
        if (epIn == null || epOut == null) return fail("no bulk endpoints")
        var communication: UsbCommunication? = null
        try {
            communication = UsbCommunicationFactory.createUsbCommunication(usb, device, iface, epOut, epIn)
            val maxLun = ByteArray(1)
            communication.controlTransfer(161, 254, 0, iface.id, maxLun, 1)
            // Card readers expose one LUN per slot; take the first with a medium.
            val block = (0..maxLun[0].toInt()).firstNotNullOfOrNull { lun ->
                BlockDeviceDriverFactory.createBlockDevice(communication, lun = lun.toByte()).let {
                    try {
                        it.init()
                        it
                    } catch (_: MediaNotInserted) {
                        null
                    }
                }
            } ?: return fail("no medium in the device")
            line("device ${block.blocks * block.blockSize} ${block.blockSize}")
            if (!confirm()) return fail("cancelled")
            action(block)
        } catch (error: Exception) {
            Log.w(TAG, "USB transfer failed", error)
            // A closed socket means the client cancelled; nothing to report then.
            runCatching { fail(error.message ?: error.javaClass.simpleName) }
        } finally {
            runCatching { communication?.close() }
        }
    }

    private fun massStorageInterface(): UsbInterface? =
        (0 until device.interfaceCount).map(device::getInterface).firstOrNull {
            it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                it.interfaceSubclass == 0x06 && it.interfaceProtocol == 0x50
        }

    private fun openImage(image: File): InputStream {
        val raw = BufferedInputStream(FileInputStream(image), CHUNK)
        return when (image.extension.lowercase()) {
            "xz" -> XZInputStream(raw)
            "gz" -> GZIPInputStream(raw, CHUNK)
            else -> raw
        }
    }

    /** Uncompressed size, or -1 when it cannot be known up front (.gz). */
    private fun uncompressedSize(image: File): Long = when (image.extension.lowercase()) {
        "xz" -> runCatching {
            SeekableXZInputStream(SeekableFileInputStream(image)).use { it.length() }
        }.getOrDefault(-1L)
        "gz" -> -1L
        else -> image.length()
    }

    private fun fill(input: InputStream, array: ByteArray): Int {
        var filled = 0
        while (filled < array.size) {
            val n = input.read(array, filled, array.size - filled)
            if (n < 0) break
            filled += n
        }
        return filled
    }

    private fun progress(done: Long, total: Long) {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastProgress < PROGRESS_EVERY_MS && done != total) return
        lastProgress = now
        line("progress $done $total")
    }

    private fun line(text: String) {
        output.write("$text\n".toByteArray())
        output.flush()
    }

    private fun fail(reason: String) = line("err $reason")

    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it) }
}
