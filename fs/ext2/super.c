#include "super.h"
#include "block.h"
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

static ext2_fs_t g_ext2_fs;

ext2_fs_t *ext2_get_fs(void) { return &g_ext2_fs; }

void ext2_super_free(void)
{
    if (g_ext2_fs.bgdt) {
        kfree(g_ext2_fs.bgdt);
        g_ext2_fs.bgdt = NULL;
    }
    g_ext2_fs.mounted = false;
    ext2_block_cache_flush();
}

int ext2_super_read(disk_device_t *disk)
{
    memset(&g_ext2_fs, 0, sizeof(g_ext2_fs));
    ext2_block_cache_flush();   /* discard any cached blocks from a previous mount */

    if (!disk || !disk->present || !disk->read) {
        printf("[ext2] super: no disk\n");
        return -1;
    }

    /*
     * The superblock is always at byte offset 1024 from the filesystem start.
     * With 512-byte sectors that is LBA 2 and 3.
     */
    uint8_t raw[1024];
    if (disk->read(2, raw) != 0 || disk->read(3, raw + 512) != 0) {
        printf("[ext2] super: I/O error reading superblock\n");
        return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)raw;

    if (sb->s_magic != EXT2_MAGIC) {
        printf("[ext2] super: bad magic 0x%x (expected 0x%x)\n",
               sb->s_magic, EXT2_MAGIC);
        return -1;
    }

    if (sb->s_rev_level >= EXT2_DYNAMIC_REV) {
        uint32_t incompat = sb->s_feature_incompat & ~EXT2_SUPPORTED_INCOMPAT;
        if (incompat) {
            printf("[ext2] super: unsupported incompat features 0x%x\n",
                   (unsigned)incompat);
            return -1;
        }
    }

    uint32_t block_size = 1024u << sb->s_log_block_size;
    if (block_size < 1024 || block_size > 4096 || (block_size & 511)) {
        printf("[ext2] super: invalid block size %u\n", (unsigned)block_size);
        return -1;
    }

    uint32_t inode_size = (sb->s_rev_level >= EXT2_DYNAMIC_REV)
                          ? sb->s_inode_size
                          : EXT2_MIN_INODE_SIZE;
    if (inode_size < EXT2_MIN_INODE_SIZE || inode_size > block_size) {
        printf("[ext2] super: invalid inode size %u\n", (unsigned)inode_size);
        return -1;
    }

    if (sb->s_inodes_per_group == 0 || sb->s_blocks_per_group == 0) {
        printf("[ext2] super: zero inodes/blocks per group\n");
        return -1;
    }

    uint32_t num_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                          / sb->s_blocks_per_group;
    if (num_groups == 0) {
        printf("[ext2] super: zero block groups\n");
        return -1;
    }

    g_ext2_fs.disk             = disk;
    g_ext2_fs.block_size       = block_size;
    g_ext2_fs.sectors_per_block= block_size / 512;
    g_ext2_fs.inodes_per_group = sb->s_inodes_per_group;
    g_ext2_fs.blocks_per_group = sb->s_blocks_per_group;
    g_ext2_fs.inode_size       = inode_size;
    g_ext2_fs.first_data_block = sb->s_first_data_block;
    g_ext2_fs.num_groups       = num_groups;
    g_ext2_fs.total_inodes     = sb->s_inodes_count;
    g_ext2_fs.total_blocks     = sb->s_blocks_count;
    g_ext2_fs.rev_level        = sb->s_rev_level;

    /*
     * Read the Block Group Descriptor Table.
     *
     * bgdt_bytes is the exact number of useful bytes (num_groups × 32).
     * bgdt_alloc rounds up to a whole number of blocks — the read loop
     * below issues one full-block read per BGDT block, writing block_size
     * bytes into the buffer each time.  Allocating only bgdt_bytes would
     * cause a heap buffer overflow when block_size > bgdt_bytes (common:
     * for 1 group, bgdt_bytes = 32 but block_size = 1024).
     */
    uint32_t bgdt_block  = sb->s_first_data_block + 1;
    uint32_t bgdt_bytes  = num_groups * sizeof(ext2_bgdt_entry_t);
    uint32_t bgdt_blocks = (bgdt_bytes + block_size - 1) / block_size;
    uint32_t bgdt_alloc  = bgdt_blocks * block_size;   /* safe: covers all sector reads */

    g_ext2_fs.bgdt = kmalloc(bgdt_alloc);
    if (!g_ext2_fs.bgdt) {
        printf("[ext2] super: out of memory for BGDT (%u bytes)\n",
               (unsigned)bgdt_alloc);
        return -1;
    }

    for (uint32_t b = 0; b < bgdt_blocks; b++) {
        uint32_t  blk = bgdt_block + b;
        uint8_t  *dst = (uint8_t *)g_ext2_fs.bgdt + b * block_size;
        uint32_t  lba = blk * g_ext2_fs.sectors_per_block;
        for (uint32_t s = 0; s < g_ext2_fs.sectors_per_block; s++) {
            if (disk->read(lba + s, dst + s * 512) != 0) {
                printf("[ext2] super: I/O error reading BGDT (blk %u)\n",
                       (unsigned)blk);
                kfree(g_ext2_fs.bgdt);
                g_ext2_fs.bgdt = NULL;
                return -1;
            }
        }
    }

    g_ext2_fs.mounted = true;

    printf("EXT2: mounted %u KiB, block=%u B, %u groups, inode=%u B\n",
           (unsigned)(sb->s_blocks_count * block_size / 1024),
           (unsigned)block_size,
           (unsigned)num_groups,
           (unsigned)inode_size);
    return 0;
}
