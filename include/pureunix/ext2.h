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

/* Table of the above, for registration via vfs_mount(). */
const vfs_ops_t *ext2_vfs_ops(void);

#endif
