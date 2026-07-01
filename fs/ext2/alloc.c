#include "alloc.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include <pureunix/string.h>

/* Matches the block-size bound already enforced by ext2_super_read()
 * (1024..4096 bytes) — one stack buffer big enough for any bitmap block. */
#define EXT2_ALLOC_MAX_BLOCK 4096

int ext2_alloc_block(uint32_t *out_blk)
{
    ext2_fs_t *fs = ext2_get_fs();

    for (uint32_t g = 0; g < fs->num_groups; g++) {
        if (fs->bgdt[g].bg_free_blocks_count == 0) continue;

        uint32_t bitmap_blk = fs->bgdt[g].bg_block_bitmap;
        const uint8_t *ro = ext2_read_block(bitmap_blk);
        if (!ro) return -1;
        uint8_t bitmap[EXT2_ALLOC_MAX_BLOCK];
        memcpy(bitmap, ro, fs->block_size);

        uint32_t group_first_block = fs->first_data_block + g * fs->blocks_per_group;
        for (uint32_t i = 0; i < fs->blocks_per_group; i++) {
            uint32_t blk_no = group_first_block + i;
            if (blk_no >= fs->total_blocks) break;
            if (bitmap[i / 8] & (1u << (i % 8))) continue;

            bitmap[i / 8] |= (1u << (i % 8));
            if (ext2_write_block(bitmap_blk, bitmap) != 0) return -1;

            fs->bgdt[g].bg_free_blocks_count--;
            ext2_bgdt_write_back();
            ext2_super_adjust_free(-1, 0);

            uint8_t zero[EXT2_ALLOC_MAX_BLOCK];
            memset(zero, 0, fs->block_size);
            if (ext2_write_block(blk_no, zero) != 0) return -1;

            *out_blk = blk_no;
            return 0;
        }
    }
    return -1;
}

void ext2_free_block(uint32_t blk)
{
    ext2_fs_t *fs = ext2_get_fs();
    if (blk < fs->first_data_block) return;

    uint32_t g = (blk - fs->first_data_block) / fs->blocks_per_group;
    uint32_t i = (blk - fs->first_data_block) % fs->blocks_per_group;
    if (g >= fs->num_groups) return;

    uint32_t bitmap_blk = fs->bgdt[g].bg_block_bitmap;
    const uint8_t *ro = ext2_read_block(bitmap_blk);
    if (!ro) return;
    uint8_t bitmap[EXT2_ALLOC_MAX_BLOCK];
    memcpy(bitmap, ro, fs->block_size);

    if (!(bitmap[i / 8] & (1u << (i % 8)))) return; /* already free */
    bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
    if (ext2_write_block(bitmap_blk, bitmap) != 0) return;

    fs->bgdt[g].bg_free_blocks_count++;
    ext2_bgdt_write_back();
    ext2_super_adjust_free(1, 0);
}

int ext2_alloc_inode(uint32_t *out_ino, bool is_dir)
{
    ext2_fs_t *fs = ext2_get_fs();

    for (uint32_t g = 0; g < fs->num_groups; g++) {
        if (fs->bgdt[g].bg_free_inodes_count == 0) continue;

        uint32_t bitmap_blk = fs->bgdt[g].bg_inode_bitmap;
        const uint8_t *ro = ext2_read_block(bitmap_blk);
        if (!ro) return -1;
        uint8_t bitmap[EXT2_ALLOC_MAX_BLOCK];
        memcpy(bitmap, ro, fs->block_size);

        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            uint32_t ino = g * fs->inodes_per_group + i + 1;
            if (ino > fs->total_inodes) break;
            if (bitmap[i / 8] & (1u << (i % 8))) continue;

            bitmap[i / 8] |= (1u << (i % 8));
            if (ext2_write_block(bitmap_blk, bitmap) != 0) return -1;

            fs->bgdt[g].bg_free_inodes_count--;
            if (is_dir) fs->bgdt[g].bg_used_dirs_count++;
            ext2_bgdt_write_back();
            ext2_super_adjust_free(0, -1);

            *out_ino = ino;
            return 0;
        }
    }
    return -1;
}

void ext2_free_inode(uint32_t ino, bool is_dir)
{
    ext2_fs_t *fs = ext2_get_fs();
    if (ino == 0 || ino > fs->total_inodes) return;

    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t i = (ino - 1) % fs->inodes_per_group;
    if (g >= fs->num_groups) return;

    uint32_t bitmap_blk = fs->bgdt[g].bg_inode_bitmap;
    const uint8_t *ro = ext2_read_block(bitmap_blk);
    if (!ro) return;
    uint8_t bitmap[EXT2_ALLOC_MAX_BLOCK];
    memcpy(bitmap, ro, fs->block_size);

    if (!(bitmap[i / 8] & (1u << (i % 8)))) return; /* already free */
    bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
    if (ext2_write_block(bitmap_blk, bitmap) != 0) return;

    fs->bgdt[g].bg_free_inodes_count++;
    if (is_dir && fs->bgdt[g].bg_used_dirs_count > 0) fs->bgdt[g].bg_used_dirs_count--;
    ext2_bgdt_write_back();
    ext2_super_adjust_free(0, 1);

    /* Zero the inode's on-disk contents so a stale i_mode/i_block can't be
     * misread if this number is reused before being fully reinitialized. */
    ext2_inode_t empty;
    memset(&empty, 0, sizeof(empty));
    ext2_write_inode(ino, &empty);
}
