package com.orailnoor.droiddesk.runtime

import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * One logical unit of a USB mass storage device, spoken to with SCSI over
 * Bulk-Only Transport (USB MSC BOT 1.0). Replaces libaums 0.10.0, which
 * misreported the capacity by one block and overran its 18-byte REQUEST SENSE
 * buffer when a card reader returned longer sense data ("newLimit > capacity").
 * The command flow follows libaums/EtchDroid; the code is DroidDesk's.
 *
 * Data safety: a READ/WRITE only counts when every byte moved and the CSW
 * residue is zero; a medium change during a transfer aborts instead of retrying
 * on a different card; [flush] issues SYNCHRONIZE CACHE before verification.
 */
class BotScsiDevice(
    private val connection: UsbDeviceConnection,
    private val iface: UsbInterface,
    private val epIn: UsbEndpoint,
    private val epOut: UsbEndpoint,
    val lun: Int,
) {
    class MediumNotPresent : IOException("no medium")
    class SenseError(val key: Int, val asc: Int, val ascq: Int) :
        IOException("SCSI error: sense key 0x%x, ASC 0x%02x/0x%02x".format(key, asc, ascq)) {
        val mediumAbsent get() = asc == 0x3A
        /** The card was changed or reset: anything in flight belongs to another medium. */
        val mediumChanged get() = key == 0x6 && (asc == 0x28 || asc == 0x29 || asc == 0x2A)
        /** "Becoming ready" (04/01): the only NOT READY worth waiting for. */
        val becomingReady get() = key == 0x2 && asc == 0x04 && ascq == 0x01
        /** Worth retrying while the unit is starting up. */
        val transient get() = !mediumAbsent && (key == 0x6 || becomingReady)
    }

    private class Result(val status: Int, val transferred: Int, val residue: Long)

    private companion object {
        /** USBDEVFS_CLEAR_HALT (usb_jni.c); null when the library is missing. */
        val nativeAvailable: Boolean = runCatching { System.loadLibrary("droiddesk_usb") }.isSuccess

        @JvmStatic
        external fun nativeClearHalt(fd: Int, endpoint: Int): Int

        @JvmStatic
        external fun nativeReset(fd: Int): Int

        /** USBDEVFS_BULK: transferred bytes, or -errno (EPIPE = stall, unlike bulkTransfer's -1). */
        @JvmStatic
        external fun nativeBulk(fd: Int, endpoint: Int, buffer: ByteArray, offset: Int, length: Int, timeout: Int): Int

        const val EPIPE = 32
        const val EIO = 5
        /** Upper bound for reading leftover data where a CSW was expected. */
        const val DRAIN_DEADLINE_MS = 3_000L

        const val CBW_SIGNATURE = 0x43425355
        const val CSW_SIGNATURE = 0x53425355
        const val TIMEOUT_MS = 10_000
        const val FLUSH_TIMEOUT_MS = 120_000
        const val CHUNK = 64 * 1024
        /** Bytes per READ/WRITE command; smaller commands recover faster. */
        const val MAX_COMMAND_BYTES = 256 * 1024
        /** Transport-level retries (after reset recovery) per command. */
        const val TRANSPORT_RETRIES = 3
        const val SENSE_LENGTH = 18
        const val STARTUP_DEADLINE_MS = 10_000L
    }

    var blockSize = 0
        private set
    /** Number of blocks (READ CAPACITY's last LBA + 1). */
    var blockCount = 0L
        private set
    var vendor = ""
        private set
    var product = ""
        private set

    private var tag = 1

    /** Inquiry, waits for the unit to become ready, reads the capacity. */
    fun init() {
        val inquiry = ByteArray(36)
        // A freshly attached stick may not answer the very first command (CSW read
        // -1); rawCommand already reset it, so give it a few tries.
        var got = 0
        for (attempt in 1..3) {
            try {
                got = dataCommand(byteArrayOf(0x12, 0, 0, 0, 36, 0), inquiry, 36, dirIn = true, minimum = 36)
                break
            } catch (error: TransportError) {
                if (attempt == 3) throw error
                Thread.sleep(300)
            }
        }
        if (got >= 32) {
            vendor = String(inquiry, 8, 8, Charsets.US_ASCII).trim()
            product = String(inquiry, 16, 16, Charsets.US_ASCII).trim()
        }
        // A fresh or just inserted card reports UNIT ATTENTION or "becoming
        // ready" for a while; anything else, including no medium, is final.
        startup { command(ByteArray(6), null, 0, 0, dirIn = false) }
        startup { readCapacity() }
        if (blockSize <= 0 || blockSize % 512 != 0 || blockSize > CHUNK) {
            throw IOException("unsupported block size $blockSize")
        }
        if (blockCount <= 0 || blockCount > Long.MAX_VALUE / blockSize) throw IOException("unsupported capacity")
    }

    private fun startup(step: () -> Unit) {
        val deadline = android.os.SystemClock.elapsedRealtime() + STARTUP_DEADLINE_MS
        while (true) {
            try {
                step()
                return
            } catch (error: SenseError) {
                if (error.mediumAbsent) throw MediumNotPresent()
                if (!error.transient || android.os.SystemClock.elapsedRealtime() > deadline) throw error
                Thread.sleep(200)
            }
        }
    }

    private fun readCapacity() {
        val capacity = ByteArray(8)
        dataCommand(byteArrayOf(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0), capacity, 8, dirIn = true, minimum = 8)
        val c = ByteBuffer.wrap(capacity).order(ByteOrder.BIG_ENDIAN)
        val lastLba10 = c.int.toLong() and 0xFFFFFFFFL
        val size10 = c.int
        if (lastLba10 == 0xFFFFFFFFL) {
            // Larger than 2 TiB at 512 B: READ CAPACITY(16).
            val capacity16 = ByteArray(32)
            val cdb = ByteArray(16).apply { this[0] = 0x9E.toByte(); this[1] = 0x10; this[13] = 32 }
            dataCommand(cdb, capacity16, 32, dirIn = true, minimum = 12)
            val c16 = ByteBuffer.wrap(capacity16).order(ByteOrder.BIG_ENDIAN)
            val lastLba = c16.long
            if (lastLba < 0) throw IOException("unsupported capacity")
            blockCount = lastLba + 1
            blockSize = c16.int
        } else {
            blockCount = lastLba10 + 1
            blockSize = size10
        }
    }

    val capacity: Long get() = blockCount * blockSize

    fun read(lba: Long, buffer: ByteArray, offset: Int, length: Int) = transfer(lba, buffer, offset, length, dirIn = true)

    fun write(lba: Long, buffer: ByteArray, offset: Int, length: Int) = transfer(lba, buffer, offset, length, dirIn = false)

    /** SYNCHRONIZE CACHE(10) for the whole medium; devices without a cache may reject it. */
    fun flush() {
        try {
            command(ByteArray(10).apply { this[0] = 0x35 }, null, 0, 0, dirIn = false, timeout = FLUSH_TIMEOUT_MS)
        } catch (error: SenseError) {
            // Cheap controllers reject SYNCHRONIZE CACHE with all sorts of ILLEGAL
            // REQUEST codes (5/20 opcode, 5/24 CDB field on abcd:1234, 5/26 parameter
            // list on an Alcor 058f:6387). Like Linux sd, take any of them as "no
            // write cache"; the read-back verify still checks the data.
            if (error.key != 0x5) throw error
        }
    }

    private fun transfer(lba: Long, buffer: ByteArray, offset: Int, length: Int, dirIn: Boolean) {
        require(length % blockSize == 0) { "length must be whole blocks" }
        if (lba < 0 || lba + length / blockSize > blockCount) throw IOException("access beyond the end of the device")
        val step = MAX_COMMAND_BYTES - MAX_COMMAND_BYTES % blockSize
        var done = 0
        while (done < length) {
            val part = minOf(step, length - done)
            transferOnce(lba + done / blockSize, buffer, offset + done, part, dirIn)
            done += part
        }
    }

    /** One READ/WRITE command; both are idempotent, so a transport error is retried after reset. */
    private fun transferOnce(lba: Long, buffer: ByteArray, offset: Int, length: Int, dirIn: Boolean) {
        val blocks = length / blockSize
        val cdb = if (lba + blocks <= 0xFFFFFFFFL && blocks <= 0xFFFF) {
            ByteBuffer.allocate(10).order(ByteOrder.BIG_ENDIAN).apply {
                put(if (dirIn) 0x28 else 0x2A); put(0); putInt(lba.toInt()); put(0); putShort(blocks.toShort()); put(0)
            }.array()
        } else {
            ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN).apply {
                put(if (dirIn) 0x88.toByte() else 0x8A.toByte()); put(0); putLong(lba); putInt(blocks); put(0); put(0)
            }.array()
        }
        var attempts = 0
        var transportErrors = 0
        while (true) {
            try {
                val result = rawCommand(cdb, buffer, offset, length, dirIn)
                checkStatus(result)
                if (result.transferred != length || result.residue != 0L) {
                    throw TransportError("incomplete transfer: ${result.transferred} of $length bytes, residue ${result.residue}")
                }
                return
            } catch (error: SenseError) {
                // Never continue on another card; only a plain "becoming ready" is retried.
                if (error.mediumChanged || error.mediumAbsent) throw IOException("the card was removed or changed", error)
                if (!error.becomingReady || ++attempts >= 5) throw error
                Thread.sleep(100)
            } catch (error: TransportError) {
                // rawCommand already did reset recovery for a broken CSW or phase error.
                if (++transportErrors > TRANSPORT_RETRIES) {
                    throw IOException("${error.message} (block $lba, $length bytes, ${if (dirIn) "read" else "write"}, after $TRANSPORT_RETRIES retries incl. port reset)", error)
                }
                // Behind the DeX dock's hub the reader sometimes stops answering on
                // bulk OUT altogether; neither clearing halts nor the Bulk-Only reset
                // brings it back. Reset the port only before the last attempt, as
                // Linux usb-storage does: a reset disturbs the hub and the other
                // devices on it, and the resynchronization above fixes most errors.
                if (transportErrors >= TRANSPORT_RETRIES) portReset() else Thread.sleep(200)
            }
        }
    }

    /**
     * USBDEVFS_RESET, then claim the interface again (the reset unbinds it) and wait
     * until the unit is ready. The device keeps its address and its medium.
     */
    private fun portReset() {
        if (!nativeAvailable) throw RecoveryFailed("port reset unavailable")
        val result = nativeReset(connection.fileDescriptor)
        android.util.Log.w("BotScsiDevice", "Port reset of the USB device: $result")
        if (result != 0) throw RecoveryFailed("port reset failed (errno ${-result})")
        Thread.sleep(500)
        if (!connection.claimInterface(iface, true)) throw IOException("could not claim the interface after a port reset")
        startup { command(ByteArray(6), null, 0, 0, dirIn = false) }
    }

    /** A USB-level failure (bad CSW, phase error, incomplete data) that a retry may fix. */
    private class TransportError(message: String) : IOException(message)

    /** Recovery itself failed: no further command may be sent; the device needs replugging. */
    class RecoveryFailed(message: String) :
        IOException("$message — the USB device does not recover; eject it and plug it in again")

    /** A command whose data length may be shorter than requested (INQUIRY, capacity). */
    private fun dataCommand(cdb: ByteArray, data: ByteArray, length: Int, dirIn: Boolean, minimum: Int): Int {
        val result = rawCommand(cdb, data, 0, length, dirIn)
        checkStatus(result)
        if (result.transferred < minimum) throw IOException("short response (${result.transferred} bytes)")
        return result.transferred
    }

    private fun command(cdb: ByteArray, data: ByteArray?, offset: Int, length: Int, dirIn: Boolean, timeout: Int = TIMEOUT_MS) {
        checkStatus(rawCommand(cdb, data, offset, length, dirIn, timeout))
    }

    private fun checkStatus(result: Result) {
        when (result.status) {
            0 -> return
            1 -> throw requestSense()
            else -> throw TransportError("phase error (CSW status ${result.status}, residue ${result.residue})")
        }
    }

    /**
     * CBW, data phase, CSW. A phase error (status 2) triggers reset recovery here,
     * for every command including REQUEST SENSE (BOT 5.3.3.1, 5.3.4).
     */
    private fun rawCommand(
        cdb: ByteArray,
        data: ByteArray?,
        offset: Int,
        length: Int,
        dirIn: Boolean,
        timeout: Int = TIMEOUT_MS,
    ): Result {
        val currentTag = tag++
        val cbw = ByteBuffer.allocate(31).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(CBW_SIGNATURE); putInt(currentTag); putInt(length)
            put(if (dirIn) 0x80.toByte() else 0); put(lun.toByte()); put(cdb.size.toByte()); put(cdb)
        }.array()
        val sentAt = android.os.SystemClock.elapsedRealtime()
        val sent = bulk(epOut, cbw, 0, 31, TIMEOUT_MS)
        if (sent != 31) {
            val took = android.os.SystemClock.elapsedRealtime() - sentAt
            recover()
            throw TransportError("could not send the SCSI command (bulk OUT ${describe(sent)} after $took ms)")
        }
        var done = 0
        if (length > 0 && data != null) {
            val ep = if (dirIn) epIn else epOut
            while (done < length) {
                val want = minOf(CHUNK, length - done)
                val n = bulk(ep, data, offset + done, want, timeout)
                if (n < 0) {
                    android.util.Log.w("BotScsiDevice", "data phase: bulk ${if (dirIn) "IN" else "OUT"} ${describe(n)} " +
                        "at $done of $length bytes (tag $currentTag)")
                    if (n == -EPIPE) {
                        // A stall: the CSW follows once the halt is cleared (BOT 6.7.2, 6.7.3).
                        if (!clearHalt(ep)) throw RecoveryFailed("clearing the endpoint halt failed")
                        break
                    }
                    // Timeout, protocol error, disconnect: the device is somewhere inside
                    // the data phase. Clearing a halt that is not there loses the step;
                    // Bulk-Only reset recovery instead, then retry the command.
                    recover()
                    throw TransportError("data phase ${describe(n)} ($done of $length bytes)")
                }
                done += n
                if (n < want) {
                    if (!dirIn) {
                        // The device took less than offered without stalling: not a BOT state.
                        recover()
                        throw TransportError("bulk OUT took $n of $want bytes")
                    }
                    // A short or zero-length packet ends the data phase (BOT 6.7.2). Judged
                    // per transfer: a ZLP after full packets leaves n packet aligned.
                    break
                }
            }
        }
        // Room for a whole packet: anything longer than 13 bytes here is a
        // protocol error, not a buffer overflow.
        val csw = ByteArray(maxOf(13, epIn.maxPacketSize))
        var got = bulk(epIn, csw, 0, csw.size, timeout)
        if (got == -EPIPE) {
            if (!clearHalt(epIn)) throw RecoveryFailed("clearing the endpoint halt failed")
            got = bulk(epIn, csw, 0, csw.size, timeout)
        }
        var c = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN)
        var signature = c.int
        var cswTag = c.int
        if (dirIn && got == csw.size && signature != CSW_SIGNATURE) {
            // Data where the CSW should be: the data phase ended early on our side. Read
            // the rest (bounded in bytes and time) until the CSW with this command's tag
            // arrives; it then goes through the normal status check below, and the short
            // transfer makes the caller retry the command without a reset.
            android.util.Log.w("BotScsiDevice", "data instead of CSW after $done of $length bytes (tag $currentTag), draining")
            var budget = length.toLong() - done + csw.size
            val deadline = android.os.SystemClock.elapsedRealtime() + DRAIN_DEADLINE_MS
            while (budget > 0 && got == csw.size && !(signature == CSW_SIGNATURE && cswTag == currentTag)) {
                val left = deadline - android.os.SystemClock.elapsedRealtime()
                if (left <= 0) break
                budget -= got
                got = bulk(epIn, csw, 0, csw.size, minOf(timeout.toLong(), left).toInt())
                if (got < 0) break
                c = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN)
                signature = c.int
                cswTag = c.int
            }
        }
        if (got != 13 || signature != CSW_SIGNATURE || cswTag != currentTag) {
            recover()
            throw TransportError(
                "invalid command status (%s, signature %08x, tag %d/%d)".format(describe(got), signature, cswTag, currentTag),
            )
        }
        val residue = c.int.toLong() and 0xFFFFFFFFL
        val status = csw[12].toInt() and 0xFF
        if (status >= 2 || residue > length) {
            recover()
            return Result(2, done, residue)
        }
        return Result(status, done, residue)
    }

    private fun bulk(ep: UsbEndpoint, buffer: ByteArray, offset: Int, length: Int, timeout: Int): Int =
        if (nativeAvailable) {
            nativeBulk(connection.fileDescriptor, ep.address, buffer, offset, length, timeout)
        } else {
            connection.bulkTransfer(ep, buffer, offset, length, timeout).let { if (it < 0) -EIO else it }
        }

    private fun describe(result: Int): String = when {
        result >= 0 -> "$result bytes"
        result == -EPIPE -> "stall"
        result == -110 -> "timeout"
        result == -19 -> "device gone"
        result == -71 -> "protocol error"
        else -> "errno ${-result}"
    }

    /** REQUEST SENSE with exactly 18 bytes; longer sense data is cut by the device. */
    private fun requestSense(): IOException {
        val sense = ByteArray(SENSE_LENGTH)
        val result = rawCommand(byteArrayOf(0x03, 0, 0, 0, SENSE_LENGTH.toByte(), 0), sense, 0, SENSE_LENGTH, dirIn = true)
        if (result.status != 0) return IOException("REQUEST SENSE failed (status ${result.status})")
        val responseCode = sense[0].toInt() and 0x7F
        if (result.transferred < 14 || (responseCode != 0x70 && responseCode != 0x71)) {
            return IOException("unreadable sense data")
        }
        return SenseError(sense[2].toInt() and 0x0F, sense[12].toInt() and 0xFF, sense[13].toInt() and 0xFF)
    }

    /**
     * Clears an endpoint halt on both sides. A bare CLEAR_FEATURE(ENDPOINT_HALT)
     * resets only the device's data toggle; the host keeps its own, the two
     * disagree, and every later transfer on that pipe fails (seen on the Fold 7 as
     * "could not send the SCSI command" that no retry fixed). USBDEVFS_CLEAR_HALT
     * does both, like the kernel's usb_clear_halt.
     */
    private fun clearHalt(ep: UsbEndpoint): Boolean =
        nativeAvailable && nativeClearHalt(connection.fileDescriptor, ep.address) == 0

    /** Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4); false if any step failed. */
    private fun resetRecovery(): Boolean {
        if (connection.controlTransfer(0x21, 0xFF, 0, iface.id, null, 0, TIMEOUT_MS) < 0) return false
        return clearHalt(epIn) && clearHalt(epOut)
    }

    /** Reset recovery that must work: otherwise no further command goes to the device. */
    private fun recover() {
        if (!resetRecovery()) throw RecoveryFailed("Bulk-Only reset recovery failed")
    }
}
