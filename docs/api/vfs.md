# API Reference: VFS

**Header**: `<pureunix/vfs.h>`

All path arguments must be absolute (begin with `/`). Every function returns `0` on success and a **negative errno** on failure (`-ENOENT`, `-EACCES`, `-ELOOP`, `-EEXIST`, `-ENOTEMPTY`, `-EXDEV`, `-EROFS`, `-EISDIR`, `-ENOTDIR`, `-EPERM`, ...) — this became uniform in Stage 4; earlier stages had a handful of calls that only returned bare `0`/`-1`. `vfs_last_error()` additionally returns a human-readable description string for shell error messages.

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
| `/` | EXT2 (ATA primary slave, read/write since Stage 4) | Primary root filesystem |
| `/fat` | FAT16 (ATA primary master, read/write) | Compatibility/testing only — Stage 4 did not change it |

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
    // Stage 4 — symlinks/hardlinks. NULL on FAT16 (no such concept there).
    int (*readlink)(const char *path, char *buf, size_t bufsize);
    int (*link)(const char *old_path, const char *new_path);
    int (*symlink)(const char *target, const char *path);
} vfs_ops_t;
```

EXT2's table now fills every slot except `chmod`/`chown` (still `-EROFS` — Stage 4 didn't add mutable ownership/permission storage, only content and directory-structure writes). FAT16's table is completely unchanged from Stage 3A.

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

### `vfs_stat` / `vfs_lstat`

```c
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_lstat(const char *path, vfs_stat_t *st);
```

Both resolve `path` through the pathname resolver (see "Pathname resolution and symlinks" below) and fill `*st`. `vfs_stat` follows a symlink named by the *final* path component (ancestors are always followed, in both functions); `vfs_lstat` does not — if the final component is itself a symlink, `*st` describes the symlink inode (`type == VFS_SYMLINK`, `st_mode` has `S_IFLNK`), not whatever it points to. This is the only difference between them.

Returns `0`, or `-ENOENT`/`-EACCES`/`-ELOOP` from resolution.

### `vfs_read_file`

```c
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
```

Resolves `path` (following a trailing symlink, so reading through one is transparent), checks `R_OK` on the resolved file, then calls the driver's `read_file`. On success, allocates a buffer via `kmalloc`, writes its address to `*out_data` and its size to `*out_size`. The caller must free the buffer with `kfree`. On EXT2, a successful read also updates the file's `atime` (see "EXT2 write path" in `docs/filesystem.md`).

Returns `0`, or a negative errno.

### `vfs_readdir`

```c
int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

Resolves `path` (following a trailing symlink to a directory, like `cd`), checks `R_OK`, then calls `cb` once per entry. Stops if `cb` returns non-zero.

`.` and `..` are ordinary entries here — no filtering happens at this layer. A caller that wants to hide them (like the shell's plain `ls`, as opposed to `ls -a`) filters them out itself.

### `vfs_write_file` / `vfs_create` / `vfs_mkdir` / `vfs_unlink` / `vfs_rmdir` / `vfs_rename` / `vfs_link` / `vfs_symlink`

```c
int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int vfs_create(const char *path);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_link(const char *old_path, const char *new_path);
int vfs_symlink(const char *target, const char *path);
```

Every write-side call checks `W_OK` on whichever directory the operation actually modifies (the parent, for create/mkdir/unlink/rmdir/rename/link/symlink; the target file itself if it already exists, for `vfs_write_file`).

Which final component gets followed differs by call, matching POSIX:

| Call | Final-component symlink handling |
|---|---|
| `vfs_write_file` | followed (write-through-symlink works, mirroring `vfs_read_file`) |
| `vfs_create` / `vfs_mkdir` / `vfs_symlink` / `vfs_link` (new name) | followed, purely to detect an existing target — `-EEXIST` either way |
| `vfs_unlink` / `vfs_rmdir` / `vfs_rename` (both paths) / `vfs_link` (old name) | **not** followed — these name the entry itself, never its target |

`vfs_write_file`: if `flags & VFS_O_APPEND`, appends; otherwise replaces existing content; creates the file first if it doesn't exist.

`vfs_rename`: both paths must resolve to the *same* mount, or the call fails with `-EXDEV` (this is checked with the fully-resolved paths, so a symlinked ancestor that redirects across a mount boundary is still caught correctly). The destination, if it already exists, is replaced: same-type only (file-for-file, empty-directory-for-empty-directory), the moved inode's number is unchanged, and `-ENOTEMPTY`/-`EISDIR`/`-ENOTDIR` cover the mismatched cases. EXT2's `ext2_rename` (see `docs/filesystem.md`) implements the actual entry-replacement and `..`-repointing logic; the VFS layer here only handles resolution, permission checks, and the cross-mount guard.

`vfs_link`: refuses to hard-link a directory (`-EPERM`), and — like rename — requires both paths to resolve to the same mount (`-EXDEV` otherwise).

`vfs_symlink`: `target` is stored verbatim, unresolved and unvalidated — a symlink may legitimately point at nothing.

### `vfs_readlink`

```c
int vfs_readlink(const char *path, char *buf, size_t bufsize);
```

`path` must itself be a symlink (its final component is never followed to get here — same resolution policy as `vfs_lstat`). Copies at most `bufsize` raw bytes of the target into `buf` — **never** NUL-terminated, exactly like POSIX `readlink(2)`. Returns the number of bytes copied (`>= 0`), or `-EINVAL` if `path` isn't a symlink, or `-ENOENT`/`-EACCES`/`-ELOOP` from resolving its ancestors.

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

## Pathname resolution and symlinks (Stage 4)

Every public entry point above is built on one internal resolver in `fs/vfs.c` (`resolve_path`, `static`, not exported) — pathname resolution, including symlink-following, exists in exactly one place, and no filesystem driver ever sees an unresolved symlink-containing path.

`resolve_path(path, out, out_size, follow_final, uid, gid)` walks `path` component by component, maintaining the absolute path resolved so far:

- **Every ancestor directory is followed through any symlink it names.** `X_OK` is required on each directory actually consulted along the way (root bypasses, as always — see "Permissions" below).
- **The final component is followed only if `follow_final` is true.** Callers that name an entity itself rather than its target (`lstat`, `readlink`, `unlink`, `rmdir`, `rename`'s and `link`'s "old" side) pass `false`; everything else (`stat`, `open`/`read_file`, `readdir`, `write_file`, and the "does this name already exist" check inside `create`/`mkdir`/`symlink`/`link`'s "new" side) passes `true`.
- **Absolute symlink targets** (`/etc/passwd`) reset resolution to `/` and continue from there. **Relative targets** (`../docs/readme`) resolve relative to the symlink's own parent directory — never the caller's current working directory — exactly like Unix.
- **Loop detection**: a counter increments on every symlink actually followed; exceeding **40** follows returns `-ELOOP` immediately rather than recursing or looping forever. This catches both direct two-node cycles (`A -> B`, `B -> A`) and longer chains.
- **A dangling final component is not a resolution error.** If the last component doesn't exist on disk, `resolve_path` still succeeds, returning the would-be path — it's each caller's job to `stat` that result and decide whether non-existence is expected (`create`/`mkdir`/`symlink`) or fatal (`open`/`unlink`/...).

Symlink target bytes themselves are fetched via each driver's `ops->readlink` — a symlink's own mode bits are never consulted for this (conventionally `0777` and universally ignored, matching every real Unix).

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

### `vfs_chmod` / `vfs_chown`

```c
int vfs_chmod(const char *path, mode_t mode);
int vfs_chown(const char *path, uid_t uid, gid_t gid);
```

Resolve the path and confirm it exists (`-ENOENT` if not). If the mount's `ops->chmod`/`ops->chown` is `NULL` (FAT16 — no on-disk Unix ownership/permission bits exist there at all), returns `-EROFS`. Otherwise runs the traditional Unix permission rule directly (`current_uid()`, same pattern as `vfs_access()`'s callers): `chmod` requires being the file's owner or root (`-EPERM` otherwise); `chown` requires being root outright — PureUNIX's one-uid/one-gid-per-task model has no supplementary-group concept that would let a non-root owner hand a file to a group they belong to, so unlike a real Unix, a non-root caller can never successfully `chown` *any* file, even one they own. Only then does it call through to the filesystem's own `ops->chmod`/`ops->chown`. EXT2 (`fs/ext2/mount.c`'s `ext2_chmod()`/`ext2_chown()`) mutates the target inode's `i_mode`/`i_uid`/`i_gid` in place and persists it to disk; `chmod` preserves the inode's file-type bits regardless of what's passed, and `chown`'s uid/gid accept `(uid_t)-1`/`(gid_t)-1` to mean "leave this one unchanged" (POSIX `chown(2)`'s convention).

### What is *not* here yet

No ACLs, no capabilities, no setuid/setgid, no sticky bit, no groups database, no login/passwd. Every process is uid 0 in practice until a login mechanism exists.
