# API Reference: VFS

**Header**: `<pureunix/vfs.h>`

All path arguments must be absolute (begin with `/`). All functions return `0` on success and `-1` on failure. On failure, `vfs_last_error()` returns a description string.

The VFS dispatches operations across two filesystems:

- **EXT2** (ATA primary slave, read-only): tried first for stat and file reads; always tried for readdir when the path is a directory in EXT2.
- **FAT16** (ATA primary master, read/write): fallback for stat and file reads; always tried for readdir when the path is a directory in FAT16; exclusive for all write operations.

---

## Constants

```c
#define VFS_O_APPEND  0x01   // append to existing file content
#define VFS_O_TRUNC   0x02   // truncate and replace existing content
```

---

## Types

### `vfs_node_type_t`

```c
typedef enum vfs_node_type {
    VFS_FILE = 1,
    VFS_DIR  = 2,
} vfs_node_type_t;
```

### `vfs_stat_t`

```c
typedef struct vfs_stat {
    vfs_node_type_t type;   // VFS_FILE or VFS_DIR
    uint32_t        size;   // file size in bytes; 0 for directories
    uint16_t        mode;   // FAT attribute byte; 0 for EXT2 entries
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

Initializes VFS internal state. Does not mount a filesystem. Always returns `0`.

### `vfs_mounted`

```c
bool vfs_mounted(void);
```

Returns `true` if at least one filesystem (EXT2 or FAT16) is currently mounted.

### `vfs_mount_root`

```c
int vfs_mount_root(void);
```

Returns `0` if at least one filesystem is already mounted, `-1` otherwise. The actual mounts are performed before `vfs_mount_root` is called (in `kernel_main`).

### `vfs_stat`

```c
int vfs_stat(const char *path, vfs_stat_t *st);
```

Fills `*st` with metadata for `path`. Tries EXT2 first; falls back to FAT16 if EXT2 does not have the path. Returns `-1` if the path does not exist on either filesystem.

For EXT2 entries, `st->mode` is 0 (EXT2 mode bits are not surfaced in this field).

### `vfs_read_file`

```c
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
```

Reads the entire contents of a file. Tries EXT2 first; falls back to FAT16. On success, allocates a buffer via `kmalloc`, writes its address to `*out_data` and its size (excluding null terminator) to `*out_size`. The buffer contains a null terminator after the data. The caller must free the buffer with `kfree`.

Returns `-1` if the path is a directory, does not exist on either filesystem, or allocation fails.

### `vfs_readdir`

```c
int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

Enumerates all visible entries in the directory at `path`. Calls `cb` with a `vfs_dirent_t` for each entry. Stops if `cb` returns non-zero.

**Union semantics**: if both EXT2 and FAT16 have a directory at `path`, both are enumerated. EXT2 entries are delivered first, then FAT16 entries. This is necessary because the two filesystems partition the namespace — for example, `ls /` must show both EXT2 data files and FAT16 program directories (`/bin`).

Returns `0` if at least one filesystem served the directory. Returns `-1` only if neither filesystem has a directory at `path`.

### `vfs_write_file`

```c
int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
```

Writes `size` bytes from `data` to `path`. Routes exclusively to FAT16.

- If `flags & VFS_O_APPEND`: prepends the existing file content.
- If `flags & VFS_O_TRUNC` or flags is 0: replaces existing content.
- If the file does not exist, it is created first.

Returns `-1` on failure or if FAT16 is not mounted.

### `vfs_create`

```c
int vfs_create(const char *path);
```

Creates a new empty regular file on FAT16. Returns `-1` if the file already exists or the parent directory does not exist.

### `vfs_mkdir`

```c
int vfs_mkdir(const char *path);
```

Creates a new directory on FAT16. The parent directory must exist. Initializes `.` and `..` entries. Returns `-1` on failure.

### `vfs_unlink`

```c
int vfs_unlink(const char *path);
```

Deletes a regular file from FAT16. Frees the cluster chain and marks the directory entry deleted. Returns `-1` if path is a directory or does not exist.

### `vfs_rmdir`

```c
int vfs_rmdir(const char *path);
```

Deletes an empty directory from FAT16. Fails if the directory contains any entries other than `.` and `..`. Returns `-1` on failure.

### `vfs_rename`

```c
int vfs_rename(const char *old_path, const char *new_path);
```

Renames or moves a file or directory within FAT16. The destination name must not already exist. Works across directories. Returns `-1` on failure.

### `vfs_last_error`

```c
const char *vfs_last_error(void);
```

Returns a static string describing the last VFS error. Valid until the next VFS call. Never returns NULL.

### `vfs_normalize`

```c
void vfs_normalize(char *out, const char *cwd, const char *path);
```

Resolves `path` into an absolute path written to `out` (max `PUREUNIX_MAX_PATH` bytes):

- If `path` begins with `/`, it is treated as absolute; `cwd` is ignored.
- Otherwise, `path` is appended to `cwd`.
- `.` components are skipped.
- `..` components pop the last path segment; clamped at root.
