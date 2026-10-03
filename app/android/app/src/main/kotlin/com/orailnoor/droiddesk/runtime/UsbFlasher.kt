package com.orailnoor.droiddesk.runtime

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
    /** Claimed and kept by UsbBridge until "eject"; this class never closes it. */
    private val held: UsbBridge.HeldDevice,
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
        /** Writes go out as whole pages of this size (see writePages). */
        const val PAGE = 4096
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

    /**
     * "blk": random block access for libdroiddesk_blk.so and droiddesk-usb (format, files).
     * After the device line, binary requests until the client sends Q or goes away:
     * op(1: R/W/F/Q) lba(8, big endian) count(4, big endian) [count blocks of data for W];
     * reply status(1: 0 ok) + the blocks for R, or len(2) + UTF-8 message on an error.
     */
    fun serveBlocks(input: InputStream) = withBlockDevice { block ->
        val bs = block.blockSize
        val requests = java.io.DataInputStream(BufferedInputStream(input, 1 shl 16))
        val replies = java.io.DataOutputStream(java.io.BufferedOutputStream(output, 1 shl 16))
        val buffer = ByteArray(maxOf(CHUNK / bs, 1) * bs)
        while (true) {
            checkCancelled()
            val op = try {
                requests.readUnsignedByte()
            } catch (_: java.io.EOFException) {
                break
            }
            val lba = requests.readLong()
            val count = requests.readInt()
            if (op == 'Q'.code) break
            val bytes = count.toLong() * bs
            if (op != 'F'.code && (count <= 0 || bytes > buffer.size)) {
                // A write's data cannot be skipped reliably: end the session.
                replyError(replies, "bad request: $count blocks")
                break
            }
            if (op == 'W'.code) requests.readFully(buffer, 0, bytes.toInt())
            try {
                when (op) {
                    'R'.code -> {
                        block.read(lba, buffer, 0, bytes.toInt())
                        replies.writeByte(0)
                        replies.write(buffer, 0, bytes.toInt())
                    }
                    'W'.code -> {
                        writePages(block, lba, buffer, bytes.toInt())
                        replies.writeByte(0)
                    }
                    'F'.code -> {
                        block.flush()
                        replies.writeByte(0)
                    }
                    else -> {
                        replyError(replies, "unknown operation $op")
                        break
                    }
                }
            } catch (error: Exception) {
                // End the session: after a card change or a transport error the device's
                // capacity and the client's cache may be stale. A new session starts clean.
                Log.w(TAG, "blk ${op.toChar()} $lba+$count failed", error)
                replyError(replies, error.message ?: error.javaClass.simpleName)
                break
            }
            replies.flush()
        }
        replies.flush()
    }

    /**
     * Writes whole 4 KiB pages: the neighbouring blocks of a partial page are read and
     * written along. A Samsung flash drive (090c:1000) acknowledged a 512-byte write and
     * returned the new sector to a one-block read, but a longer read of that area came from
     * the old page (mtools then did not see a folder it had just created; SYNCHRONIZE
     * CACHE did not help). With no partial page writes, no page gets into that state.
     */
    private fun writePages(block: BotScsiDevice, lba: Long, data: ByteArray, length: Int) {
        val bs = block.blockSize
        val per = if (bs < PAGE) (PAGE / bs).toLong() else 1L
        val count = (length / bs).toLong()
        val start = lba / per * per
        val end = minOf((lba + count + per - 1) / per * per, block.blockCount)
        if (per == 1L || (start == lba && end == lba + count) || end < lba + count) {
            block.write(lba, data, 0, length)
            return
        }
        val page = ByteArray(((end - start) * bs).toInt())
        // The edges are shorter than a page, and were themselves written as whole pages.
        if (start < lba) block.read(start, page, 0, ((lba - start) * bs).toInt())
        val tail = lba + count
        if (end > tail) block.read(tail, page, ((tail - start) * bs).toInt(), ((end - tail) * bs).toInt())
        System.arraycopy(data, 0, page, ((lba - start) * bs).toInt(), length)
        block.write(start, page, 0, page.size)
    }

    private fun replyError(replies: java.io.DataOutputStream, message: String) {
        val text = message.toByteArray().let { if (it.size > 1000) it.copyOf(1000) else it }
        replies.writeByte(1)
        replies.writeShort(text.size)
        replies.write(text)
        replies.flush()
    }

    private fun checkCancelled() {
        if (isCancelled()) throw java.io.IOException("cancelled: DroidDesk is shutting down")
    }

    private fun withBlockDevice(
        precheck: (capacity: Long) -> String? = { null },
        action: (BotScsiDevice) -> Unit,
    ) {
        val connection = held.connection
        val iface = held.iface
        val epIn = held.epIn
        val epOut = held.epOut
        // One operation at a time per device.
        synchronized(held) {
        try {
            // GET MAX LUN; devices with one LUN may stall it, which means 0. Android
            // cannot tell a stall from a transient error, so ask twice before
            // assuming one LUN. Multi-slot readers must answer it (BOT 3.2), so a
            // silent single-LUN fallback only affects devices that never do.
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
            fun probe() = (0 until lunCount).mapNotNull { slot ->
                BotScsiDevice(connection, iface, epIn, epOut, slot).let {
                    try {
                        it.init()
                        slot to it
                    } catch (_: BotScsiDevice.MediumNotPresent) {
                        null
                    }
                }
            }
            // Right after attach a card reader may still report "no medium" for the
            // inserted card (seen on a 2-slot reader); ask once more before giving up.
            val media = probe().ifEmpty {
                Thread.sleep(700)
                probe()
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
        }
        }
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
