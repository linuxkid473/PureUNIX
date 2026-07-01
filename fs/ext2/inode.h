#ifndef EXT2_INODE_H
#define EXT2_INODE_H

#include "ext2.h"

/* Read inode ino from the inode table into *out.
   ino is 1-based; ino==0 is invalid.
   Returns 0 on success, -1 on error. */
int ext2_read_inode(uint32_t ino, ext2_inode_t *out);

/* Write *in back to inode ino's slot in the inode table. Returns 0 on
   success, -1 on error (out-of-range ino or I/O failure). Stage 4: every
   write-path operation (create, unlink, link count changes, timestamp
   updates, directory growth) goes through this one function. */
int ext2_write_inode(uint32_t ino, const ext2_inode_t *in);

/* Record new_blk as the logical_index'th data block of inode (0-based).
   Transparently allocates and links a singly-indirect block the first time
   logical_index reaches 12. Does not persist inode — caller must still
   ext2_write_inode() afterwards. Returns 0 on success, -1 if logical_index
   is beyond what singly-indirect addressing can reach or on I/O error. */
int ext2_inode_add_block(ext2_inode_t *inode, uint32_t logical_index, uint32_t new_blk);

/* Free every data block (and, if present, the singly-indirect block itself)
   referenced by inode, resetting i_block/i_size/i_blocks to zero. Caller
   must still ext2_write_inode() (or ext2_free_inode(), which zeroes the
   whole inode) afterwards. */
void ext2_inode_free_all_blocks(ext2_inode_t *inode);

/* Callback type for ext2_iter_blocks. cb receives each data block number in
   order.  Return non-zero to stop iteration early. */
typedef int (*ext2_block_cb_t)(uint32_t blk_no, void *ctx);

/* Iterate through the data blocks of an inode, calling cb for each block
   number until nbytes of file data have been covered (or the inode ends).
   Handles direct blocks [0..11] and singly-indirect block [12].
   Double/triple indirect are skipped (not supported in Stage 1).
   Returns 0 when iteration finishes normally, or a non-zero cb return value
   if the callback stopped it early. */
int ext2_iter_blocks(const ext2_inode_t *inode, uint32_t nbytes,
                     ext2_block_cb_t cb, void *ctx);

#endif
