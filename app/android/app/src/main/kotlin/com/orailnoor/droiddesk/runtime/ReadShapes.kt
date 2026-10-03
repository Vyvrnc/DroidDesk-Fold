package com.orailnoor.droiddesk.runtime

/**
 * READ commands a device has seen, by shape (start block, block count), and which of them
 * a later write made stale.
 *
 * A Samsung flash drive (090c:1000) caches the data of a READ by its exact shape and does
 * not invalidate that entry when the blocks are written: repeating the same READ returns
 * the old data, any other shape (one block earlier, one shorter) returns the current data,
 * and Force Unit Access is ignored. mtools reads a folder with the same shape before and
 * after creating an entry in it, so a just created folder "did not exist".
 *
 * Kept per device (and slot) while it stays plugged in: the cache lives as long as the
 * device has power, also across bridge sessions and eject/attach. Whole aligned commands
 * of [step] blocks (sequential reads of a whole disk) are kept as bits, other shapes in a map.
 */
class ReadShapes(private val step: Int) {
    private val regularRead = java.util.BitSet()
    private val regularStale = java.util.BitSet()
    /** Other shapes: start block -> block counts. */
    private val issued = java.util.TreeMap<Long, MutableSet<Int>>()
    private val stale = HashSet<Long>()
    private var longest = 0

    private fun key(start: Long, count: Int) = (start shl 20) or count.toLong()

    private fun regular(start: Long, count: Int) = count == step && start % step == 0L && start / step < Int.MAX_VALUE

    @Synchronized
    fun isStale(start: Long, count: Int): Boolean =
        if (regular(start, count)) regularStale[(start / step).toInt()] else key(start, count) in stale

    @Synchronized
    fun read(start: Long, count: Int) {
        if (regular(start, count)) {
            regularRead.set((start / step).toInt())
            return
        }
        issued.getOrPut(start) { HashSet() }.add(count)
        if (count > longest) longest = count
    }

    /** Every shape read so far that covers part of [start, end) is stale now. */
    @Synchronized
    fun written(start: Long, end: Long) {
        val first = (start / step).toInt()
        val last = ((end - 1) / step).toInt()
        for (index in first..last) if (regularRead[index]) regularStale.set(index)
        val from = maxOf(0L, start - longest)
        for ((readStart, counts) in issued.subMap(from, true, end, false)) {
            for (count in counts) {
                if (readStart + count > start) stale.add(key(readStart, count))
            }
        }
    }
}
