#ifndef PUREUNIX_VFS_H
#define PUREUNIX_VFS_H

#include <pureunix/config.h>
#include <pureunix/types.h>

#define VFS_O_APPEND 0x01
#define VFS_O_TRUNC  0x02

typedef enum vfs_node_type {
    VFS_FILE = 1,
    VFS_DIR = 2,
    VFS_SYMLINK = 3,
} vfs_node_type_t;

typedef struct vfs_stat {
    vfs_node_type_t type;
    uint32_t size;
    uint16_t mode;   /* legacy FS-specific attribute/permission byte */

    /* Unix metadata. Populated directly from the on-disk inode where the
     * filesystem has one (EXT2); synthesized with sensible defaults where
     * it doesn't (FAT16 — see fat16_stat's comment). No permission checks
     * are performed anywhere against these fields yet. */
    mode_t   st_mode;   /* file-type bits | rwx permission bits */
    uid_t    st_uid;
    gid_t    st_gid;
    nlink_t  st_nlink;
    ino_t    st_ino;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_blocks;   /* 512-byte blocks allocated */
    uint32_t st_blksize;  /* preferred I/O block size */
} vfs_stat_t;

typedef struct vfs_dirent {
    char name[PUREUNIX_MAX_NAME];
    vfs_node_type_t type;
    uint32_t size;
} vfs_dirent_t;

typedef int (*vfs_readdir_cb_t)(const vfs_dirent_t *entry, void *ctx);

/* Filesystem type tag — used only for identification/display (e.g. mount,
 * df); routing never switches on it. Add an entry here when a new driver
 * gains a vfs_mount() registration. */
typedef enum vfs_fs_type {
    VFS_FS_NONE = 0,
    VFS_FS_EXT2,
    VFS_FS_FAT16,
} vfs_fs_type_t;

/* The set of operations a filesystem driver exposes to the VFS mount layer.
 * A driver that doesn't support an operation (e.g. a read-only filesystem
 * has no write_file/create/unlink/rename) leaves that pointer NULL; the VFS
 * treats a NULL op as "unsupported" and fails the call cleanly. */
typedef struct vfs_ops {
    int (*stat)(const char *path, vfs_stat_t *st);
    int (*read_file)(const char *path, uint8_t **out_data, size_t *out_size);
    int (*write_file)(const char *path, const uint8_t *data, size_t size, uint32_t flags);
    int (*create)(const char *path, bool directory);
    int (*unlink)(const char *path, bool directory);
    int (*rename)(const char *old_path, const char *new_path);
    int (*readdir)(const char *path, vfs_readdir_cb_t cb, void *ctx);
    /* Both NULL on every driver today — no filesystem stores mutable
     * ownership/permission bits yet. Present so a future writable
     * filesystem can plug in without any VFS-level API change. */
    int (*chmod)(const char *path, mode_t mode);
    int (*chown)(const char *path, uid_t uid, gid_t gid);
} vfs_ops_t;

typedef struct vfs_mount {
    char mountpoint[PUREUNIX_MAX_PATH];
    vfs_fs_type_t type;
    const vfs_ops_t *ops;
    void *fs_private;
} vfs_mount_t;

/* Register (or replace) a mount at an absolute path. Paths are resolved by
 * longest-prefix match against the mount table, so mounts may nest (e.g.
 * "/" and "/fat" can coexist; "/fat/..." resolves to the "/fat" mount). */
int vfs_mount(const char *mountpoint, vfs_fs_type_t type, const vfs_ops_t *ops, void *fs_private);

/* Longest-prefix mount lookup. Returns NULL if no mount covers path. */
const vfs_mount_t *vfs_find_mount(const char *path);

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

/* -------------------------------------------------------------------------
 * Permissions (Stage 3A)
 * ---------------------------------------------------------------------- */

/* access(2) mode bits. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* The one permission engine every read/write/traverse/exec check in the
 * kernel goes through — no permission logic is duplicated anywhere else.
 * Evaluates owner bits first, then group, then other (never combined:
 * matching the owner means only the owner bits apply, even if group/other
 * would have been more permissive). uid 0 (root) bypasses read/write checks
 * entirely and bypasses execute checks as long as at least one of the
 * S_IXUSR/S_IXGRP/S_IXOTH bits is set on st_mode. */
bool vfs_access(const vfs_stat_t *st, uid_t uid, gid_t gid, int requested);

/* Syscall-infrastructure-only stubs: no filesystem driver implements mutable
 * ownership/permission storage yet (EXT2 is read-only; FAT16 has none), so
 * both currently return -EROFS for any existing path, or -ENOENT if the
 * path doesn't exist. */
int vfs_chmod(const char *path, mode_t mode);
int vfs_chown(const char *path, uid_t uid, gid_t gid);

#endif
