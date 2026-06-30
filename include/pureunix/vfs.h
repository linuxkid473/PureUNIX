#ifndef PUREUNIX_VFS_H
#define PUREUNIX_VFS_H

#include <pureunix/config.h>
#include <pureunix/types.h>

#define VFS_O_APPEND 0x01
#define VFS_O_TRUNC  0x02

typedef enum vfs_node_type {
    VFS_FILE = 1,
    VFS_DIR = 2,
} vfs_node_type_t;

typedef struct vfs_stat {
    vfs_node_type_t type;
    uint32_t size;
    uint16_t mode;
} vfs_stat_t;

typedef struct vfs_dirent {
    char name[PUREUNIX_MAX_NAME];
    vfs_node_type_t type;
    uint32_t size;
} vfs_dirent_t;

typedef int (*vfs_readdir_cb_t)(const vfs_dirent_t *entry, void *ctx);

int vfs_init(void);
bool vfs_mounted(void);
int vfs_mount_root(void);
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int vfs_create(const char *path);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
const char *vfs_last_error(void);
void vfs_normalize(char *out, const char *cwd, const char *path);

#endif
