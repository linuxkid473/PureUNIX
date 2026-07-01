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

/* -------------------------------------------------------------------------
 * Directory modification (Stage 4)
 * ---------------------------------------------------------------------- */

/* Insert one new (name, ino, file_type) entry into directory dir_ino.
   Reuses a deleted slot or splits a used entry's trailing slack space if
   either is big enough; otherwise allocates and appends a fresh directory
   block. Updates dir_ino's mtime/ctime. Returns 0 on success, -1 on error
   (not a directory, name too long, allocation failure). Does not check for
   an existing entry with the same name — callers must do that themselves. */
int ext2_dir_insert(uint32_t dir_ino, const char *name, uint32_t ino, uint8_t file_type);

/* Remove the entry named name from directory dir_ino. Merges its rec_len
   into the previous entry in the same block (or, if it is the first entry
   in its block, simply marks it unused via inode=0) so the block's entry
   chain is never corrupted. Updates dir_ino's mtime/ctime. Returns 0 on
   success, -1 if name is not found. */
int ext2_dir_remove(uint32_t dir_ino, const char *name);

/* True if dir_ino contains nothing but "." and "..". Used by rmdir() (and
   rename() when replacing an existing destination directory) to refuse
   removing/replacing a non-empty directory. */
bool ext2_dir_is_empty(uint32_t dir_ino);

/* Rewrite dir_ino's ".." entry to point at new_parent_ino. Used by rename()
   when moving a directory to a new parent — the moved directory's notion
   of its own parent must follow it. Returns 0 on success, -1 if dir_ino has
   no ".." entry (should not happen for any directory this driver creates). */
int ext2_dir_set_parent(uint32_t dir_ino, uint32_t new_parent_ino);

#endif
