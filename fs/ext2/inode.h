#ifndef EXT2_INODE_H
#define EXT2_INODE_H

#include "ext2.h"

/* Read inode ino from the inode table into *out.
   ino is 1-based; ino==0 is invalid.
   Returns 0 on success, -1 on error. */
int ext2_read_inode(uint32_t ino, ext2_inode_t *out);

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
