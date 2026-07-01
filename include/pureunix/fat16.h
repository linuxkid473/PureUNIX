#ifndef PUREUNIX_FAT16_H
#define PUREUNIX_FAT16_H

#include <pureunix/disk.h>
#include <pureunix/vfs.h>

int fat16_mount(disk_device_t *disk);
bool fat16_is_mounted(void);
int fat16_stat(const char *path, vfs_stat_t *st);
int fat16_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int fat16_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int fat16_create(const char *path, bool directory);
int fat16_unlink(const char *path, bool directory);
int fat16_rename(const char *old_path, const char *new_path);
int fat16_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
uint32_t fat16_free_bytes(void);
uint32_t fat16_total_bytes(void);

/* Table of the above, for registration via vfs_mount(). */
const vfs_ops_t *fat16_vfs_ops(void);

#endif
