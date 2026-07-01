#ifndef EXT2_ALLOC_H
#define EXT2_ALLOC_H

#include <pureunix/types.h>

/* Block/inode bitmap allocator (Stage 4). Every allocation/free updates the
 * relevant group's bitmap block, the in-memory + on-disk BGDT counters
 * (bg_free_blocks_count/bg_free_inodes_count/bg_used_dirs_count), and the
 * superblock's aggregate free counts — so the filesystem stays internally
 * consistent and durable across remounts, exactly as real ext2 does. */

/* Allocate a free data block, zero its contents, and mark it used. Returns
   0 with *out_blk set, or -1 if the filesystem has no free blocks. */
int ext2_alloc_block(uint32_t *out_blk);

/* Return a previously-allocated data block to the free pool. */
void ext2_free_block(uint32_t blk);

/* Allocate a free inode number and mark it used. is_dir adjusts the owning
   group's bg_used_dirs_count. Returns 0 with *out_ino set, or -1 if the
   filesystem has no free inodes. Does not initialize the inode's contents —
   callers must write a fully-populated inode via ext2_write_inode(). */
int ext2_alloc_inode(uint32_t *out_ino, bool is_dir);

/* Return a previously-allocated inode number to the free pool and zero its
   on-disk contents so a stale i_mode can't linger past reuse. */
void ext2_free_inode(uint32_t ino, bool is_dir);

#endif
