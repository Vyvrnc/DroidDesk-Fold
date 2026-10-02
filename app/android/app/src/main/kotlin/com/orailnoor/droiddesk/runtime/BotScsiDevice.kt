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
        IOException("SCSI error: sense key 0x%x, ASC 0x%02x/0x%02x".format(key, asc, ascq))

    private companion object {
        const val CBW_SIGNATURE = 0x43425355
        const val CSW_SIGNATURE = 0x53425355
        const val TIMEOUT_MS = 10_000
        const val CHUNK = 64 * 1024
        const val SENSE_LENGTH = 18
        const val RETRIES = 5
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
        command(byteArrayOf(0x12, 0, 0, 0, 36, 0), inquiry, 0, 36, dirIn = true)
        vendor = String(inquiry, 8, 8, Charsets.US_ASCII).trim()
        product = String(inquiry, 16, 16, Charsets.US_ASCII).trim()

        // TEST UNIT READY; a fresh or just inserted card reports UNIT ATTENTION first.
        var lastError: IOException? = null
        for (attempt in 0 until RETRIES) {
            try {
                command(ByteArray(6), null, 0, 0, dirIn = false)
                lastError = null
                break
            } catch (error: SenseError) {
                if (error.key == 0x2 && error.asc == 0x3A) throw MediumNotPresent()
                lastError = error
                Thread.sleep(200)
            }
        }
        lastError?.let { throw it }

        val capacity = ByteArray(8)
        command(byteArrayOf(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0), capacity, 0, 8, dirIn = true)
        val c = ByteBuffer.wrap(capacity).order(ByteOrder.BIG_ENDIAN)
        val lastLba10 = c.int.toLong() and 0xFFFFFFFFL
        blockSize = c.int
        if (lastLba10 == 0xFFFFFFFFL) {
            // Larger than 2 TiB at 512 B: READ CAPACITY(16).
            val capacity16 = ByteArray(32)
            val cdb = ByteArray(16).apply { this[0] = 0x9E.toByte(); this[1] = 0x10; this[13] = 32 }
            command(cdb, capacity16, 0, 32, dirIn = true)
            val c16 = ByteBuffer.wrap(capacity16).order(ByteOrder.BIG_ENDIAN)
            blockCount = c16.long + 1
            blockSize = c16.int
        } else {
            blockCount = lastLba10 + 1
        }
        if (blockSize <= 0 || blockSize % 512 != 0) throw IOException("unsupported block size $blockSize")
    }

    val capacity: Long get() = blockCount * blockSize

    fun read(lba: Long, buffer: ByteArray, offset: Int, length: Int) = transfer(lba, buffer, offset, length, dirIn = true)

    fun write(lba: Long, buffer: ByteArray, offset: Int, length: Int) = transfer(lba, buffer, offset, length, dirIn = false)

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
        var lastError: IOException? = null
        for (attempt in 0 until RETRIES) {
            try {
                command(cdb, buffer, offset, length, dirIn)
                return
            } catch (error: SenseError) {
                // UNIT ATTENTION and NOT READY (becoming ready) are worth a retry.
                if (error.key != 0x6 && error.key != 0x2) throw error
                lastError = error
                Thread.sleep(100)
            }
        }
        throw lastError ?: IOException("transfer failed")
    }

    /** One BOT command: CBW, optional data phase, CSW. Throws SenseError on CHECK CONDITION. */
    private fun command(cdb: ByteArray, data: ByteArray?, offset: Int, length: Int, dirIn: Boolean) {
        val status = rawCommand(cdb, data, offset, length, dirIn)
        when (status) {
            0 -> return
            1 -> throw requestSense()
            else -> {
                resetRecovery()
                throw IOException("phase error (CSW status $status)")
            }
        }
    }

    private fun rawCommand(cdb: ByteArray, data: ByteArray?, offset: Int, length: Int, dirIn: Boolean): Int {
        val currentTag = tag++
        val cbw = ByteBuffer.allocate(31).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(CBW_SIGNATURE); putInt(currentTag); putInt(length)
            put(if (dirIn) 0x80.toByte() else 0); put(lun.toByte()); put(cdb.size.toByte()); put(cdb)
        }.array()
        if (connection.bulkTransfer(epOut, cbw, 31, TIMEOUT_MS) != 31) {
            resetRecovery()
            throw IOException("could not send the SCSI command")
        }
        if (length > 0 && data != null) {
            val ep = if (dirIn) epIn else epOut
            var done = 0
            while (done < length) {
                val n = connection.bulkTransfer(ep, data, offset + done, minOf(CHUNK, length - done), TIMEOUT_MS)
                if (n < 0) {
                    // A stalled data phase still ends with a CSW after the halt is cleared.
                    clearHalt(ep)
                    break
                }
                if (n == 0) break
                done += n
                // Short reads end the data phase (the residue is in the CSW).
                if (dirIn && n < minOf(CHUNK, length - done + n)) break
            }
        }
        val csw = ByteArray(13)
        var got = connection.bulkTransfer(epIn, csw, 13, TIMEOUT_MS)
        if (got < 0) {
            clearHalt(epIn)
            got = connection.bulkTransfer(epIn, csw, 13, TIMEOUT_MS)
        }
        val c = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN)
        if (got != 13 || c.int != CSW_SIGNATURE || c.int != currentTag) {
            resetRecovery()
            throw IOException("invalid command status from the device")
        }
        c.int // residue
        return csw[12].toInt() and 0xFF
    }

    /** REQUEST SENSE with exactly 18 bytes; longer sense data is cut by the device. */
    private fun requestSense(): IOException {
        val sense = ByteArray(SENSE_LENGTH)
        val status = rawCommand(byteArrayOf(0x03, 0, 0, 0, SENSE_LENGTH.toByte(), 0), sense, 0, SENSE_LENGTH, dirIn = true)
        if (status != 0) return IOException("REQUEST SENSE failed (status $status)")
        val key = sense[2].toInt() and 0x0F
        val asc = sense[12].toInt() and 0xFF
        val ascq = sense[13].toInt() and 0xFF
        return SenseError(key, asc, ascq)
    }

    private fun clearHalt(ep: UsbEndpoint) {
        // CLEAR_FEATURE(ENDPOINT_HALT) on the endpoint.
        connection.controlTransfer(0x02, 0x01, 0, ep.address, null, 0, TIMEOUT_MS)
    }

    /** Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4). */
    private fun resetRecovery() {
        connection.controlTransfer(0x21, 0xFF, 0, iface.id, null, 0, TIMEOUT_MS)
        clearHalt(epIn)
        clearHalt(epOut)
    }
}
