#include "inode.h"
#include "alloc.h"
#include "block.h"
#include "super.h"
#include <pureunix/stdio.h>
#include <pureunix/string.h>

/* Matches the block-size bound enforced by ext2_super_read() (1024..4096). */
#define EXT2_INODE_MAX_BLOCK 4096

int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    ext2_fs_t *fs = ext2_get_fs();

    if (ino == 0 || ino > fs->total_inodes) {
        printf("[ext2] inode: inode %u out of range (max %u)\n",
               (unsigned)ino, (unsigned)fs->total_inodes);
        return -1;
    }

    uint32_t group      = (ino - 1) / fs->inodes_per_group;
    uint32_t local_idx  = (ino - 1) % fs->inodes_per_group;

    if (group >= fs->num_groups) {
        printf("[ext2] inode: group %u out of range\n", (unsigned)group);
        return -1;
    }

    uint32_t byte_off     = local_idx * fs->inode_size;
    uint32_t table_block  = fs->bgdt[group].bg_inode_table;
    uint32_t block_off    = byte_off / fs->block_size;
    uint32_t off_in_block = byte_off % fs->block_size;

    /* ext2_read_block returns a non-owning pointer into the block cache.
       Do NOT call kfree() on it — the cache owns the buffer. */
    const uint8_t *blk = ext2_read_block(table_block + block_off);
    if (!blk) return -1;

    memcpy(out, blk + off_in_block, EXT2_MIN_INODE_SIZE);
    return 0;
}

/* -------------------------------------------------------------------------
 * Block iterator — visits each data block number for an inode in order.
 * ---------------------------------------------------------------------- */

int ext2_iter_blocks(const ext2_inode_t *inode, uint32_t nbytes,
                     ext2_block_cb_t cb, void *ctx)
{
    ext2_fs_t *fs = ext2_get_fs();
    uint32_t remaining = nbytes;

    /* --- Direct blocks [0..11] --- */
    for (int i = 0; i < 12 && remaining > 0; i++) {
        uint32_t blk = inode->i_block[i];
        remaining -= (remaining < fs->block_size) ? remaining : fs->block_size;
        int r = cb(blk, ctx);    /* blk == 0 means sparse hole; cb handles it */
        if (r != 0) return r;
    }

    if (remaining == 0 || inode->i_block[12] == 0) return 0;

    /* --- Singly-indirect block [12] ---
     *
     * The indirect block contains an array of block pointers.  We read it via
     * ext2_read_block(), which returns a non-owning pointer into the block
     * cache.  The callback (cb) will itself call ext2_read_block() for each
     * data block, which can evict any cache slot — including the one that
     * holds our indirect block data.
     *
     * To avoid using a stale pointer, we copy the pointer table into a local
     * array BEFORE calling any callback.  1024 B / 4 = 256 entries maximum
     * for the 1 KB blocks this driver accepts.
     */
    const uint8_t *ind_data = ext2_read_block(inode->i_block[12]);
    if (!ind_data) return -1;

    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    if (ptrs_per_block > 256) ptrs_per_block = 256;    /* safety cap */

    uint32_t local_ptrs[256];
    memcpy(local_ptrs, ind_data, ptrs_per_block * sizeof(uint32_t));
    /* ind_data is no longer referenced after this point. */

    for (uint32_t p = 0; p < ptrs_per_block && remaining > 0; p++) {
        uint32_t blk = local_ptrs[p];
        remaining -= (remaining < fs->block_size) ? remaining : fs->block_size;
        int r = cb(blk, ctx);
        if (r != 0) return r;
    }

    if (remaining == 0 || inode->i_block[13] == 0) return 0;

    /* --- Doubly-indirect block [13] ---
     *
     * i_block[13] points to a block of pointers to *singly-indirect*
     * blocks, each of which points to up to ptrs_per_block direct data
     * blocks — the same "copy the pointer table locally before calling any
     * callback" reasoning as the singly-indirect case above applies at
     * both levels, since either one could evict the other from the block
     * cache. Only needed once a file's real block count exceeds 12 +
     * ptrs_per_block (268 for 1 KB blocks) — see docs/developer-guide.md's
     * discussion of this cap and tools/mkext2.py's matching write-side
     * support.
     */
    const uint8_t *dind_data = ext2_read_block(inode->i_block[13]);
    if (!dind_data) return -1;

    uint32_t dind_ptrs[256];
    memcpy(dind_ptrs, dind_data, ptrs_per_block * sizeof(uint32_t));
    /* dind_data is no longer referenced after this point. */

    for (uint32_t d = 0; d < ptrs_per_block && remaining > 0; d++) {
        if (dind_ptrs[d] == 0) {
            /* A sparse hole at the singly-indirect level: skip a full
             * indirect block's worth of data blocks. */
            uint32_t skip = ptrs_per_block;
            while (skip > 0 && remaining > 0) {
                remaining -= (remaining < fs->block_size) ? remaining : fs->block_size;
                int r = cb(0, ctx);
                if (r != 0) return r;
                skip--;
            }
            continue;
        }

        const uint8_t *ind2_data = ext2_read_block(dind_ptrs[d]);
        if (!ind2_data) return -1;
        uint32_t ind2_ptrs[256];
        memcpy(ind2_ptrs, ind2_data, ptrs_per_block * sizeof(uint32_t));

        for (uint32_t p = 0; p < ptrs_per_block && remaining > 0; p++) {
            uint32_t blk = ind2_ptrs[p];
            remaining -= (remaining < fs->block_size) ? remaining : fs->block_size;
            int r = cb(blk, ctx);
            if (r != 0) return r;
        }
    }

    return 0;
}

int ext2_write_inode(uint32_t ino, const ext2_inode_t *in)
{
    ext2_fs_t *fs = ext2_get_fs();

    if (ino == 0 || ino > fs->total_inodes) {
        printf("[ext2] inode: write of inode %u out of range (max %u)\n",
               (unsigned)ino, (unsigned)fs->total_inodes);
        return -1;
    }

    uint32_t group     = (ino - 1) / fs->inodes_per_group;
    uint32_t local_idx = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->num_groups) return -1;

    uint32_t byte_off    = local_idx * fs->inode_size;
    uint32_t table_block = fs->bgdt[group].bg_inode_table;
    uint32_t block_off   = byte_off / fs->block_size;
    uint32_t off_in_block= byte_off % fs->block_size;

    const uint8_t *ro = ext2_read_block(table_block + block_off);
    if (!ro) return -1;
    uint8_t buf[EXT2_INODE_MAX_BLOCK];
    memcpy(buf, ro, fs->block_size);
    memcpy(buf + off_in_block, in, EXT2_MIN_INODE_SIZE);
    return ext2_write_block(table_block + block_off, buf);
}

int ext2_inode_add_block(ext2_inode_t *inode, uint32_t logical_index, uint32_t new_blk)
{
    ext2_fs_t *fs = ext2_get_fs();

    if (logical_index < 12) {
        inode->i_block[logical_index] = new_blk;
        inode->i_blocks += fs->sectors_per_block;
        return 0;
    }

    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    uint32_t ind_index = logical_index - 12;
    if (ind_index >= ptrs_per_block) {
        printf("[ext2] inode: logical block %u exceeds singly-indirect range\n",
               (unsigned)logical_index);
        return -1; /* runtime growth past singly-indirect isn't supported —
                     * unlike the read side (ext2_iter_blocks() above) and
                     * tools/mkext2.py's image-builder writer, both of which
                     * do support doubly-indirect blocks; see
                     * docs/developer-guide.md's "File Size Cap" section. */
    }

    if (inode->i_block[12] == 0) {
        uint32_t ind_blk;
        if (ext2_alloc_block(&ind_blk) != 0) return -1;
        inode->i_block[12] = ind_blk;
        inode->i_blocks += fs->sectors_per_block;
    }

    const uint8_t *ro = ext2_read_block(inode->i_block[12]);
    if (!ro) return -1;
    uint8_t buf[EXT2_INODE_MAX_BLOCK];
    memcpy(buf, ro, fs->block_size);
    memcpy(buf + ind_index * sizeof(uint32_t), &new_blk, sizeof(uint32_t));
    if (ext2_write_block(inode->i_block[12], buf) != 0) return -1;

    inode->i_blocks += fs->sectors_per_block;
    return 0;
}

void ext2_inode_free_all_blocks(ext2_inode_t *inode)
{
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            ext2_free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    if (inode->i_block[12]) {
        ext2_fs_t *fs = ext2_get_fs();
        const uint8_t *ro = ext2_read_block(inode->i_block[12]);
        if (ro) {
            uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
            uint32_t local_ptrs[EXT2_INODE_MAX_BLOCK / sizeof(uint32_t)];
            memcpy(local_ptrs, ro, ptrs_per_block * sizeof(uint32_t));
            for (uint32_t p = 0; p < ptrs_per_block; p++) {
                if (local_ptrs[p]) ext2_free_block(local_ptrs[p]);
            }
        }
        ext2_free_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }

    if (inode->i_block[13]) {
        ext2_fs_t *fs = ext2_get_fs();
        uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
        const uint8_t *ro = ext2_read_block(inode->i_block[13]);
        if (ro) {
            uint32_t dind_ptrs[EXT2_INODE_MAX_BLOCK / sizeof(uint32_t)];
            memcpy(dind_ptrs, ro, ptrs_per_block * sizeof(uint32_t));
            for (uint32_t d = 0; d < ptrs_per_block; d++) {
                if (!dind_ptrs[d]) continue;
                const uint8_t *ro2 = ext2_read_block(dind_ptrs[d]);
                if (ro2) {
                    uint32_t ind2_ptrs[EXT2_INODE_MAX_BLOCK / sizeof(uint32_t)];
                    memcpy(ind2_ptrs, ro2, ptrs_per_block * sizeof(uint32_t));
                    for (uint32_t p = 0; p < ptrs_per_block; p++) {
                        if (ind2_ptrs[p]) ext2_free_block(ind2_ptrs[p]);
                    }
                }
                ext2_free_block(dind_ptrs[d]);
            }
        }
        ext2_free_block(inode->i_block[13]);
        inode->i_block[13] = 0;
    }

    inode->i_blocks = 0;
    inode->i_size = 0;
}
