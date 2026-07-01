#include "block.h"
#include "super.h"
#include <pureunix/stdio.h>
#include <pureunix/string.h>

/*
 * Four-slot LRU block cache.  Each slot holds one full block (up to 4096 B).
 *
 * Why a cache?
 *   A single `ls /` with both EXT2 and FAT16 mounted causes `ext2_stat("/")`
 *   to be called twice (once from vfs_stat, once from vfs_readdir's internal
 *   check), plus once more inside ext2_readdir_ino.  Each call reads the root
 *   inode (inode 2), which lives in block 5 (the first inode-table block).
 *   Without a cache every such call allocates 1024 bytes, issues a disk read,
 *   and immediately frees the buffer — paying full I/O cost every time.
 *
 *   The cache eliminates all repeated disk reads AND all transient kmalloc/
 *   kfree calls in the block layer.  Callers receive a non-owning pointer into
 *   cache-owned memory; they must not call kfree() on it.
 */

#define EXT2_BLOCK_CACHE_SLOTS  4
#define EXT2_BLOCK_SIZE_MAX     4096    /* largest block size we accept */

typedef struct {
    uint32_t blk_no;                    /* 0 = empty slot                */
    uint32_t lru_seq;                   /* higher = more recently used   */
    uint8_t  data[EXT2_BLOCK_SIZE_MAX];
} block_cache_slot_t;

static block_cache_slot_t g_cache[EXT2_BLOCK_CACHE_SLOTS];
static uint32_t           g_lru_seq;   /* global LRU counter            */

void ext2_block_cache_flush(void)
{
    for (int i = 0; i < EXT2_BLOCK_CACHE_SLOTS; i++)
        g_cache[i].blk_no = 0;
    g_lru_seq = 0;
}

const uint8_t *ext2_read_block(uint32_t blk_no)
{
    ext2_fs_t *fs = ext2_get_fs();

    /* Block 0 is the padding before the superblock — never a valid request. */
    if (blk_no == 0) {
        printf("[ext2] block: read of block 0 rejected\n");
        return NULL;
    }

    /* Cache lookup — O(slots), 4 iterations. */
    for (int i = 0; i < EXT2_BLOCK_CACHE_SLOTS; i++) {
        if (g_cache[i].blk_no == blk_no) {
            g_cache[i].lru_seq = ++g_lru_seq;
            return g_cache[i].data;
        }
    }

    /* Cache miss: find the LRU slot (lowest lru_seq). */
    int victim = 0;
    for (int i = 1; i < EXT2_BLOCK_CACHE_SLOTS; i++) {
        if (g_cache[i].lru_seq < g_cache[victim].lru_seq)
            victim = i;
    }

    /* Read from disk into the chosen slot. */
    uint32_t lba = blk_no * fs->sectors_per_block;
    for (uint32_t s = 0; s < fs->sectors_per_block; s++) {
        if (fs->disk->read(lba + s, g_cache[victim].data + s * 512) != 0) {
            printf("[ext2] block: I/O error (blk=%u lba=%u)\n",
                   (unsigned)blk_no, (unsigned)(lba + s));
            g_cache[victim].blk_no = 0;    /* invalidate slot on error */
            return NULL;
        }
    }

    g_cache[victim].blk_no  = blk_no;
    g_cache[victim].lru_seq = ++g_lru_seq;
    return g_cache[victim].data;
}

int ext2_write_block(uint32_t blk_no, const uint8_t *data)
{
    ext2_fs_t *fs = ext2_get_fs();

    if (blk_no == 0) {
        printf("[ext2] block: write of block 0 rejected\n");
        return -1;
    }

    uint32_t lba = blk_no * fs->sectors_per_block;
    for (uint32_t s = 0; s < fs->sectors_per_block; s++) {
        if (fs->disk->write(lba + s, data + s * 512) != 0) {
            printf("[ext2] block: I/O error writing (blk=%u lba=%u)\n",
                   (unsigned)blk_no, (unsigned)(lba + s));
            return -1;
        }
    }

    /* Keep the cache coherent: if this block is cached, refresh its slot in
     * place rather than invalidating it, so callers holding a pointer from
     * ext2_read_block() earlier in the same operation still see valid
     * memory (they must not still be relying on its *old* contents, per the
     * existing single-outstanding-pointer discipline documented in block.h). */
    for (int i = 0; i < EXT2_BLOCK_CACHE_SLOTS; i++) {
        if (g_cache[i].blk_no == blk_no) {
            memcpy(g_cache[i].data, data, fs->block_size);
            return 0;
        }
    }
    return 0;
}
