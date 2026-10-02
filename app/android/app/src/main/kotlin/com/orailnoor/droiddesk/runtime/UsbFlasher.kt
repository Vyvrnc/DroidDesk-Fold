package com.orailnoor.droiddesk.runtime

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.util.Log
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.util.zip.GZIPInputStream

/**
 * Stable flashing path of droiddesk-usb: SCSI over Bulk-Only Transport
 * (BotScsiDevice), the approach of EtchDroid/libaums. Raw images only — the image is
 * copied block for block, nothing is interpreted (a Windows ISO needs to be
 * turned into a disk image first).
 */
class UsbFlasher(
    private val usb: UsbManager,
    private val device: UsbDevice,
    private val output: OutputStream,
    /** Card reader slot (SCSI LUN); null picks the only populated one. */
    private val lun: Int?,
    /** Checked between chunks; true once the bridge is shutting down. */
    private val isCancelled: () -> Boolean,
    /** Called after the device line; returns false to stop before any write. */
    private val confirm: () -> Boolean,
) {
    private companion object {
        const val TAG = "UsbFlasher"
        const val CHUNK = 1 shl 20
        const val PROGRESS_EVERY_MS = 500L
    }

    private var lastProgress = 0L

    fun flash(image: File) {
        if (!image.isFile) return fail("no such image ${image.path}")
        // A compressed image is decoded once in full before anything is written:
        // its size and integrity are only known at the end of the stream, and a
        // corrupt or oversized image must not get as far as the partition table.
        val (total, sourceDigest) = if (isCompressed(image)) {
            line("validate")
            measure(image) ?: return
        } else {
            image.length() to null
        }
        withBlockDevice(
            precheck = { capacity ->
                if (total > capacity) "image is $total bytes, the device only $capacity" else null
            },
        ) { block ->
            val digest = MessageDigest.getInstance("SHA-256")
            // Whole blocks, so the zero padding of the last chunk always fits.
            val buffer = ByteArray(CHUNK - CHUNK % block.blockSize)
            var written = 0L
            var lba = 0L
            openImage(image).use { input ->
                while (true) {
                    checkCancelled()
                    val read = fill(input, buffer)
                    if (read <= 0) break
                    if (written + read > total) return@withBlockDevice fail("the image changed while flashing")
                    digest.update(buffer, 0, read)
                    // The device takes whole blocks; pad the last one with zeros.
                    val padded = (read + block.blockSize - 1) / block.blockSize * block.blockSize
                    java.util.Arrays.fill(buffer, read, padded, 0)
                    block.write(lba, buffer, 0, padded)
                    lba += padded / block.blockSize
                    written += read
                    progress(written, total)
                }
            }
            if (written != total) return@withBlockDevice fail("the image changed while flashing")
            val expected = hex(digest.digest())
            if (sourceDigest != null && sourceDigest != expected) {
                return@withBlockDevice fail("the image changed while flashing")
            }
            // Out of the device's write cache first, so the read-back checks the medium.
            block.flush()
            line("verify")
            val check = MessageDigest.getInstance("SHA-256")
            val readBack = ByteArray(buffer.size)
            var verified = 0L
            lba = 0L
            while (verified < written) {
                checkCancelled()
                val want = minOf(readBack.size.toLong(), written - verified).toInt()
                val padded = (want + block.blockSize - 1) / block.blockSize * block.blockSize
                java.util.Arrays.fill(readBack, 0)
                block.read(lba, readBack, 0, padded)
                check.update(readBack, 0, want)
                lba += padded / block.blockSize
                verified += want
                progress(verified, written)
            }
            val actual = hex(check.digest())
            if (actual != expected) return@withBlockDevice fail("verify failed: wrote $expected, read back $actual")
            line("done $expected")
        }
    }

    fun readTo(target: File) = withBlockDevice { block ->
        val total = block.capacity
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(CHUNK - CHUNK % block.blockSize)
        var done = 0L
        var lba = 0L
        target.outputStream().buffered(CHUNK).use { out ->
            while (done < total) {
                checkCancelled()
                val want = minOf(buffer.size.toLong(), total - done).toInt()
                block.read(lba, buffer, 0, want)
                out.write(buffer, 0, want)
                digest.update(buffer, 0, want)
                lba += want / block.blockSize
                done += want
                progress(done, total)
            }
        }
        line("done ${hex(digest.digest())}")
    }

    private fun checkCancelled() {
        if (isCancelled()) throw java.io.IOException("cancelled: DroidDesk is shutting down")
    }

    private fun withBlockDevice(
        precheck: (capacity: Long) -> String? = { null },
        action: (BotScsiDevice) -> Unit,
    ) {
        val iface = massStorageInterface() ?: return fail("not a USB mass storage device (BOT/SCSI)")
        val endpoints = (0 until iface.endpointCount).map(iface::getEndpoint)
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val epIn = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN }
        val epOut = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT }
        if (epIn == null || epOut == null) return fail("no bulk endpoints")
        val connection = usb.openDevice(device) ?: return fail("Android could not open the device")
        try {
            // force = true detaches the kernel's usb-storage driver.
            if (!connection.claimInterface(iface, true)) return fail("could not claim the interface")
            // GET MAX LUN; devices with one LUN may stall it, which means 0. Android
            // cannot tell a stall from a transient error, so ask twice before
            // assuming one LUN (that would bypass the several-cards check).
            val maxLun = ByteArray(1)
            val lunCount = if (
                connection.controlTransfer(0xA1, 0xFE, 0, iface.id, maxLun, 1, 5_000) == 1 ||
                connection.controlTransfer(0xA1, 0xFE, 0, iface.id, maxLun, 1, 5_000) == 1
            ) {
                val value = maxLun[0].toInt() and 0xFF
                if (value > 15) return fail("the device reported an invalid number of slots ($value)")
                value + 1
            } else {
                1
            }
            // Card readers expose one LUN per slot. Never guess between two
            // inserted cards: the user has to name the slot.
            val media = (0 until lunCount).mapNotNull { slot ->
                BotScsiDevice(connection, iface, epIn, epOut, slot).let {
                    try {
                        it.init()
                        slot to it
                    } catch (_: BotScsiDevice.MediumNotPresent) {
                        null
                    }
                }
            }
            val (slot, block) = when {
                media.isEmpty() -> return fail("no medium in the device")
                lun != null -> media.firstOrNull { it.first == lun }
                    ?: return fail("no medium in slot $lun (populated: ${media.joinToString { "${it.first}" }})")
                media.size > 1 -> return fail(
                    "several cards inserted, choose one with --lun: " +
                        media.joinToString { "${it.first} (${it.second.capacity} bytes)" },
                )
                else -> media.single()
            }
            val capacity = block.capacity
            precheck(capacity)?.let { return fail(it) }
            line("device $capacity ${block.blockSize} $slot ${media.size}")
            if (!confirm()) return fail("cancelled")
            action(block)
        } catch (error: Exception) {
            Log.w(TAG, "USB transfer failed", error)
            // A closed socket means the client cancelled; nothing to report then.
            runCatching { fail(error.message ?: error.javaClass.simpleName) }
        } finally {
            runCatching { connection.releaseInterface(iface) }
            connection.close()
        }
    }

    private fun massStorageInterface(): UsbInterface? =
        (0 until device.interfaceCount).map(device::getInterface).firstOrNull {
            it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                it.interfaceSubclass == 0x06 && it.interfaceProtocol == 0x50
        }

    private fun openImage(image: File): InputStream =
        decoder(image, BufferedInputStream(FileInputStream(image), CHUNK))

    private fun decoder(image: File, raw: InputStream): InputStream =
        when (image.extension.lowercase()) {
            "xz" -> XZInputStream(raw)
            "gz" -> GZIPInputStream(raw, CHUNK)
            else -> raw
        }

    private fun isCompressed(image: File) = image.extension.lowercase() in setOf("xz", "gz")

    /**
     * Decodes the whole image without touching the device and returns its
     * uncompressed size and SHA-256, or reports the error and returns null.
     * The decoders check their own CRCs, so a truncated or corrupt file fails here.
     */
    private fun measure(image: File): Pair<Long, String>? = try {
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(CHUNK)
        var total = 0L
        val compressed = image.length()
        val counting = CountingInputStream(FileInputStream(image))
        decoder(image, BufferedInputStream(counting, CHUNK)).use { input ->
            while (true) {
                checkCancelled()
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
                total += read
                progress(counting.count, compressed)
            }
        }
        total to hex(digest.digest())
    } catch (error: Exception) {
        Log.w(TAG, "Image validation failed", error)
        fail("the image is damaged or not ${image.extension}: ${error.message ?: error.javaClass.simpleName}")
        null
    }

    private class CountingInputStream(input: InputStream) : java.io.FilterInputStream(input) {
        var count = 0L
        override fun read(): Int = super.read().also { if (it >= 0) count++ }
        override fun read(b: ByteArray, off: Int, len: Int): Int =
            super.read(b, off, len).also { if (it > 0) count += it }
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
