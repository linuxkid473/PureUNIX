#ifndef PUREUNIX_EXT2_H
#define PUREUNIX_EXT2_H

#include <pureunix/disk.h>
#include <pureunix/vfs.h>

/* Mount the EXT2 filesystem on disk.  Reads the superblock and block group
   descriptor table; returns 0 on success, -1 on any validation failure. */
int ext2_mount(disk_device_t *disk);

/* Returns true if an EXT2 filesystem is currently mounted. */
bool ext2_is_mounted(void);

/* VFS-level operations — same signature pattern as fat16_*. */
int ext2_stat(const char *path, vfs_stat_t *st);
int ext2_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int ext2_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);

/* Write path (Stage 4) — fs/ext2/write.c. Every function returns 0 or a
 * negative errno, matching vfs_ops_t's contract. */
int ext2_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int ext2_create(const char *path, bool directory);
int ext2_unlink(const char *path, bool directory);
int ext2_rename(const char *old_path, const char *new_path);
int ext2_link(const char *old_path, const char *new_path);
int ext2_symlink(const char *target, const char *path);
int ext2_readlink(const char *path, char *buf, size_t bufsize);

/* Real chmod/chown, backed by the on-disk inode's i_mode/i_uid/i_gid — see
 * fs/ext2/mount.c. uid/gid of (uid_t)-1/(gid_t)-1 mean "leave unchanged",
 * matching POSIX chown(2)'s convention (e.g. chown(path, uid, -1) to
 * change owner without touching group). */
int ext2_chmod(const char *path, mode_t mode);
int ext2_chown(const char *path, uid_t uid, gid_t gid);
/* atime/mtime of 0xFFFFFFFF means "leave that one unchanged" — see
 * include/pureunix/vfs.h's vfs_ops_t.utime. */
int ext2_utime(const char *path, uint32_t atime, uint32_t mtime);

/* Table of the above, for registration via vfs_mount(). */
const vfs_ops_t *ext2_vfs_ops(void);

#endif
