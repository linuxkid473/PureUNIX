# API Reference: VFS

**Header**: `<pureunix/vfs.h>`

All path arguments must be absolute (begin with `/`). All functions return `0` on success and `-1` on failure. On failure, `vfs_last_error()` returns a description string. (The three permission-infrastructure calls added in Stage 3A — `vfs_chmod`, `vfs_chown` — are the exception: they return a negative errno directly, since callers need to distinguish `-ENOENT` from `-EROFS`.)

## Mount Table

The VFS is a generic mount-point router, not a dual-dispatch/union layer: every path resolves to exactly one mount via longest-prefix match, and the resolved mount's driver serves the call. There is no merging of directory listings across filesystems.

```c
typedef struct vfs_mount {
    char             mountpoint[PUREUNIX_MAX_PATH];
    vfs_fs_type_t    type;         // identification/display only — routing never switches on it
    const vfs_ops_t *ops;
    void            *fs_private;
} vfs_mount_t;

int vfs_mount(const char *mountpoint, vfs_fs_type_t type, const vfs_ops_t *ops, void *fs_private);
const vfs_mount_t *vfs_find_mount(const char *path);
```

Current mounts, registered in `kernel_main` after each driver's own `_mount()` call succeeds:

| Mountpoint | Driver | Notes |
|---|---|---|
| `/` | EXT2 (ATA primary slave, read-only) | Primary root filesystem |
| `/fat` | FAT16 (ATA primary master, read/write) | Compatibility/testing only |

Adding a future filesystem requires only a new driver plus a `vfs_mount()` call — no changes to the dispatch logic below.

### `vfs_ops_t`

The set of operations a driver exposes. A driver that doesn't implement an operation leaves that pointer `NULL`; the VFS treats a `NULL` op as "unsupported."

```c
typedef struct vfs_ops {
    int (*stat)(const char *path, vfs_stat_t *st);
    int (*read_file)(const char *path, uint8_t **out_data, size_t *out_size);
    int (*write_file)(const char *path, const uint8_t *data, size_t size, uint32_t flags);
    int (*create)(const char *path, bool directory);
    int (*unlink)(const char *path, bool directory);
    int (*rename)(const char *old_path, const char *new_path);
    int (*readdir)(const char *path, vfs_readdir_cb_t cb, void *ctx);
    int (*chmod)(const char *path, mode_t mode);   // NULL on every driver today
    int (*chown)(const char *path, uid_t uid, gid_t gid); // NULL on every driver today
} vfs_ops_t;
```

---

## Constants

```c
#define VFS_O_APPEND  0x01   // append to existing file content
#define VFS_O_TRUNC   0x02   // truncate and replace existing content

// access(2)-style mode bits, used by vfs_access() and SYS_ACCESS (Stage 3A)
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
```

---

## Types

### `vfs_node_type_t`

```c
typedef enum vfs_node_type {
    VFS_FILE    = 1,
    VFS_DIR     = 2,
    VFS_SYMLINK = 3,
} vfs_node_type_t;
```

### `vfs_stat_t`

```c
typedef struct vfs_stat {
    vfs_node_type_t type;
    uint32_t        size;
    uint16_t        mode;      // legacy FS-specific attribute/permission byte

    // Unix metadata (Stage 2C/2D). Direct from the on-disk inode on EXT2;
    // synthesized with sensible defaults on FAT16 (uid=gid=0, mode derived
    // from file/dir type, nlink=1) since it has no such storage.
    mode_t   st_mode;    // file-type bits | rwx permission bits
    uid_t    st_uid;
    gid_t    st_gid;
    nlink_t  st_nlink;
    ino_t    st_ino;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_blocks;   // 512-byte blocks allocated
    uint32_t st_blksize;  // preferred I/O block size
} vfs_stat_t;
```

### `vfs_dirent_t`

```c
typedef struct vfs_dirent {
    char            name[PUREUNIX_MAX_NAME];  // null-terminated, ≤63 chars
    vfs_node_type_t type;
    uint32_t        size;
} vfs_dirent_t;
```

### `vfs_readdir_cb_t`

```c
typedef int (*vfs_readdir_cb_t)(const vfs_dirent_t *entry, void *ctx);
```

Callback for `vfs_readdir`. Return non-zero to stop iteration early.

---

## Functions

### `vfs_init`

```c
int vfs_init(void);
```

Initializes VFS internal state (empties the mount table). Always returns `0`.

### `vfs_mounted`

```c
bool vfs_mounted(void);
```

Returns `true` if at least one mount is registered.

### `vfs_mount` / `vfs_find_mount`

See "Mount Table" above.

### `vfs_stat`

```c
int vfs_stat(const char *path, vfs_stat_t *st);
```

Resolves `path` to its mount (checking `X_OK` on every ancestor directory along the way — see "Permissions" below) and fills `*st`. Returns `-1` if the path doesn't resolve to a mount, the driver has no path there, or a traversal permission check fails.

### `vfs_read_file`

```c
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
```

Reads the entire contents of a file: checks ancestor `X_OK`, then `R_OK` on the file itself, then calls the resolved driver's `read_file`. On success, allocates a buffer via `kmalloc`, writes its address to `*out_data` and its size to `*out_size`. The caller must free the buffer with `kfree`.

Returns `-1` if the path doesn't resolve, isn't a regular file the driver will read, or either permission check fails.

### `vfs_readdir`

```c
int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

Enumerates all entries in the directory at `path` through its resolved mount: checks ancestor `X_OK`, then `R_OK` on the directory itself, then calls `cb` once per entry. Stops if `cb` returns non-zero.

`.` and `..` are ordinary entries here — no filtering happens at this layer (see "Permissions" and EXT2's directory notes). A caller that wants to hide them (like the shell's plain `ls`, as opposed to `ls -a`) filters them out itself.

Returns `0` on success, `-1` if the path doesn't resolve to a directory or a permission check fails.

### `vfs_write_file` / `vfs_create` / `vfs_mkdir` / `vfs_unlink` / `vfs_rmdir` / `vfs_rename`

```c
int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int vfs_create(const char *path);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
```

Every write-side call checks ancestor `X_OK` first, then `W_OK` on whichever directory the operation actually modifies (the parent, for create/mkdir/unlink/rmdir/rename; the target file itself if it already exists, for `vfs_write_file`). This runs unconditionally even though EXT2 is read-only (its ops table has these slots `NULL`, so they simply fail after the permission check) — a future writable filesystem gets enforcement for free.

`vfs_write_file`: if `flags & VFS_O_APPEND`, appends; if `flags & VFS_O_TRUNC` or `flags == 0`, replaces existing content; creates the file first if it doesn't exist.

`vfs_rename`: both old and new paths must resolve to the *same* mount.

### `vfs_last_error`

```c
const char *vfs_last_error(void);
```

Returns a static string describing the last VFS error (including permission denials and missing ancestor directories). Valid until the next VFS call. Never returns NULL.

### `vfs_normalize`

```c
void vfs_normalize(char *out, const char *cwd, const char *path);
```

Resolves `path` into an absolute path written to `out` (max `PUREUNIX_MAX_PATH` bytes):

- If `path` begins with `/`, it is treated as absolute; `cwd` is ignored.
- Otherwise, `path` is appended to `cwd`.
- `.` components are skipped.
- `..` components pop the last path segment; clamped at root.

Purely lexical — does not consult the filesystem, so it works the same whether or not the resulting path actually exists.

---

## Permissions (Stage 3A)

### Process credentials

Every `task_t` carries `uid_t uid` and `gid_t gid` (see `docs/api/task.md`). The kernel task and everything descended from it start as `uid=0, gid=0` (root); there is no login system, no additional users, and no passwd database yet. `task_create()` copies the creating task's credentials into the new task, so credentials "inherit" the only way they currently can.

```c
uid_t current_uid(void);   // task_current()->uid, or 0 if there is no current task
gid_t current_gid(void);   // task_current()->gid, or 0 if there is no current task
```

Every VFS entry point above calls these two internally — callers never pass credentials explicitly.

### `vfs_access` — the permission engine

```c
bool vfs_access(const vfs_stat_t *st, uid_t uid, gid_t gid, int requested);
```

The single reusable permission check; every read/write/traverse/execute gate in the kernel (the VFS functions above, `SYS_ACCESS`, `SYS_OPEN`, `elf_exec`) calls this — no permission bit-twiddling is duplicated anywhere else.

- `requested` is an OR of `R_OK`/`W_OK`/`X_OK` (or exactly `F_OK` — always `true`, since existence was already established by the caller's own `stat`).
- **uid 0 (root)**: read and write are always granted. Execute is granted only if `st_mode` has at least one of `S_IXUSR`/`S_IXGRP`/`S_IXOTH` set — traditional Unix behaviour (root can't execute a file nobody can execute).
- **Non-root**: owner bits apply if `st->st_uid == uid`; otherwise group bits apply if `st->st_gid == gid`; otherwise other bits apply. These are mutually exclusive tiers, not combined — matching the file's owner means *only* the owner bits are consulted, even if group/other would have been more permissive.

### `vfs_chmod` / `vfs_chown` — syscall infrastructure only

```c
int vfs_chmod(const char *path, mode_t mode);
int vfs_chown(const char *path, uid_t uid, gid_t gid);
```

Resolve the path and confirm it exists (`-ENOENT` if not), then call the mount's `ops->chmod`/`ops->chown`. Both are `NULL` on every driver today (EXT2 is read-only; FAT16 has no permission storage), so every existing path currently gets `-EROFS`. No actual chmod/chown behaviour exists yet — this is scaffolding so a future writable filesystem can plug in without any VFS-level API change.

### What is *not* here yet

No ACLs, no capabilities, no setuid/setgid, no sticky bit, no groups database, no login/passwd. Every process is uid 0 in practice until a login mechanism exists.
