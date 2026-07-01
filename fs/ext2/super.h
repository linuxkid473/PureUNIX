#ifndef EXT2_SUPER_H
#define EXT2_SUPER_H

#include "ext2.h"
#include <pureunix/disk.h>

/* Returns a pointer to the global EXT2 state.  All other ext2 modules call
   this rather than maintaining their own copy of the pointer. */
ext2_fs_t *ext2_get_fs(void);

/* Read and validate the superblock from disk; populate g_ext2_fs.
   Returns 0 on success, negative on error. */
int ext2_super_read(disk_device_t *disk);

/* Release resources held by the global state (BGDT buffer, etc.). */
void ext2_super_free(void);

/* Apply block_delta/inode_delta to the on-disk superblock's
 * s_free_blocks_count/s_free_inodes_count (positive when freeing, negative
 * when allocating) and write the two affected sectors straight back to
 * disk. Used exclusively by the block/inode allocator (fs/ext2/alloc.c) so
 * every mutation of free-space bookkeeping goes through one place. */
void ext2_super_adjust_free(int32_t block_delta, int32_t inode_delta);

/* Persist the in-memory BGDT (g_ext2_fs.bgdt) back to disk. Called by the
 * allocator after it updates bg_free_blocks_count/bg_free_inodes_count/
 * bg_used_dirs_count in memory. */
void ext2_bgdt_write_back(void);

#endif
