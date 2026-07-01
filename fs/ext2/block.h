#ifndef EXT2_BLOCK_H
#define EXT2_BLOCK_H

#include <pureunix/types.h>

/*
 * Block cache — 4 LRU slots, each large enough for the maximum supported
 * block size (4096 bytes).  Cache ownership:
 *   - Returned pointers are non-owning; callers must NOT call kfree() on them.
 *   - A returned pointer is valid until the next ext2_read_block() call that
 *     causes an eviction.  In single-threaded cooperative code, this means the
 *     pointer is safe for the duration of the current synchronous operation,
 *     provided no *other* ext2_read_block() call is made while holding it.
 *   - If a caller needs to hold block data across another ext2_read_block()
 *     call (e.g. iterating an indirect block while also reading data blocks),
 *     it must copy the needed data into a local buffer first.
 */

/* Read EXT2 block blk_no from disk.  Returns a pointer into the block cache
   (non-owning — do NOT kfree).  Returns NULL on I/O error or if blk_no == 0.
   Repeated calls for the same blk_no return the cached copy without I/O. */
const uint8_t *ext2_read_block(uint32_t blk_no);

/* Invalidate all cache slots.  Must be called on unmount or remount so stale
   data from the previous disk is not returned for the same block number. */
void ext2_block_cache_flush(void);

#endif
