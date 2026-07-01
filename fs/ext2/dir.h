#ifndef EXT2_DIR_H
#define EXT2_DIR_H

#include <pureunix/types.h>
#include <pureunix/vfs.h>

/* Look up a single name component in the directory with inode dir_ino.
   On success, writes the found inode number into *out_ino and returns 0.
   Returns -1 if the entry is not found or an I/O error occurs. */
int ext2_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *out_ino);

/* Resolve an absolute path (must start with '/') to an inode number.
   Returns 0 and writes *out_ino on success; returns -1 on failure. */
int ext2_path_to_inode(const char *path, uint32_t *out_ino);

/* Iterate directory entries for dir_ino, calling cb for each visible entry.
   Follows the same vfs_readdir_cb_t convention: cb returns non-zero to stop.
   Returns 0 on normal completion, non-zero if cb stopped early. */
int ext2_readdir_ino(uint32_t dir_ino, vfs_readdir_cb_t cb, void *ctx);

#endif
