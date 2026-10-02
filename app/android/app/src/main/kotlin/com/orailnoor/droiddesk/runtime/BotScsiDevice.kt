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
        /** Worth retrying while the unit is starting up. */
        val transient get() = !mediumAbsent && (key == 0x6 || (key == 0x2 && asc == 0x04))
    }

    private class Result(val status: Int, val transferred: Int, val residue: Long)

    private companion object {
        const val CBW_SIGNATURE = 0x43425355
        const val CSW_SIGNATURE = 0x53425355
        const val TIMEOUT_MS = 10_000
        const val FLUSH_TIMEOUT_MS = 120_000
        const val CHUNK = 64 * 1024
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
        val got = dataCommand(byteArrayOf(0x12, 0, 0, 0, 36, 0), inquiry, 36, dirIn = true, minimum = 36)
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
            // ILLEGAL REQUEST = no write cache to flush; anything else is a real failure.
            if (error.key != 0x5) throw error
        }
    }

    private fun transfer(lba: Long, buffer: ByteArray, offset: Int, length: Int, dirIn: Boolean) {
        require(length % blockSize == 0) { "length must be whole blocks" }
        val blocks = length / blockSize
        if (lba < 0 || lba + blocks > blockCount) throw IOException("access beyond the end of the device")
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
        while (true) {
            try {
                val result = rawCommand(cdb, buffer, offset, length, dirIn)
                checkStatus(result)
                if (result.transferred != length || result.residue != 0L) {
                    throw IOException("incomplete transfer at block $lba: ${result.transferred} of $length bytes, residue ${result.residue}")
                }
                return
            } catch (error: SenseError) {
                // Never continue on another card; only a plain "becoming ready" is retried.
                if (error.mediumChanged || error.mediumAbsent) throw IOException("the card was removed or changed", error)
                if (!(error.key == 0x2 && error.asc == 0x04) || ++attempts >= 5) throw error
                Thread.sleep(100)
            }
        }
    }

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
            else -> throw IOException("phase error (CSW status ${result.status})")
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
        if (connection.bulkTransfer(epOut, cbw, 31, TIMEOUT_MS) != 31) {
            resetRecovery()
            throw IOException("could not send the SCSI command")
        }
        var done = 0
        if (length > 0 && data != null) {
            val ep = if (dirIn) epIn else epOut
            while (done < length) {
                val want = minOf(CHUNK, length - done)
                val n = connection.bulkTransfer(ep, data, offset + done, want, timeout)
                if (n < 0) {
                    // Usually a stall; the CSW still follows once the halt is cleared.
                    clearHalt(ep)
                    break
                }
                done += n
                // A short packet ends the data phase (the CSW carries the residue).
                if (n < want) break
            }
        }
        val csw = ByteArray(13)
        var got = connection.bulkTransfer(epIn, csw, 13, timeout)
        if (got < 0) {
            clearHalt(epIn)
            got = connection.bulkTransfer(epIn, csw, 13, timeout)
        }
        val c = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN)
        if (got != 13 || c.int != CSW_SIGNATURE || c.int != currentTag) {
            resetRecovery()
            throw IOException("invalid command status from the device")
        }
        val residue = c.int.toLong() and 0xFFFFFFFFL
        val status = csw[12].toInt() and 0xFF
        if (status >= 2 || residue > length) {
            resetRecovery()
            return Result(2, done, residue)
        }
        return Result(status, done, residue)
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

    private fun clearHalt(ep: UsbEndpoint) {
        // CLEAR_FEATURE(ENDPOINT_HALT). Android has no API for the host-side
        // toggle reset (USBDEVFS_CLEAR_HALT); BOT devices cope with this in practice.
        connection.controlTransfer(0x02, 0x01, 0, ep.address, null, 0, TIMEOUT_MS)
    }

    /** Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4). */
    private fun resetRecovery() {
        connection.controlTransfer(0x21, 0xFF, 0, iface.id, null, 0, TIMEOUT_MS)
        clearHalt(epIn)
        clearHalt(epOut)
    }
}
