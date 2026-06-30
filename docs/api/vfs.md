# API Reference: VFS

**Header**: `<pureunix/vfs.h>`

All path arguments must be absolute (begin with `/`). All functions return `0` on success and `-1` on failure. On failure, `vfs_last_error()` returns a description string.

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
    uint16_t        mode;   // FAT attribute byte
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

Callback for `vfs_readdir`. Return non-zero to stop iteration.

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

Returns `true` if a filesystem is currently mounted.

### `vfs_mount_root`

```c
int vfs_mount_root(void);
```

Attempts to mount the FAT16 filesystem on the ATA primary master. Returns `0` on success.

### `vfs_stat`

```c
int vfs_stat(const char *path, vfs_stat_t *st);
```

Fills `*st` with metadata for `path`. Returns `-1` if path does not exist or filesystem is not mounted.

### `vfs_read_file`

```c
int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
```

Reads the entire contents of a file. On success, allocates a buffer via `kmalloc`, writes its address to `*out_data` and its size (excluding null terminator) to `*out_size`. The buffer contains a null terminator after the data. The caller must free the buffer with `kfree`.

Returns `-1` if the path is a directory, does not exist, or allocation fails.

### `vfs_write_file`

```c
int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
```

Writes `size` bytes from `data` to `path`.

- If `flags & VFS_O_APPEND`: prepends the existing file content.
- If `flags & VFS_O_TRUNC` or flags is 0: replaces existing content.
- If the file does not exist, it is created first.

Returns `-1` on failure.

### `vfs_create`

```c
int vfs_create(const char *path);
```

Creates a new empty regular file. Returns `-1` if the file already exists or the parent directory does not exist.

### `vfs_mkdir`

```c
int vfs_mkdir(const char *path);
```

Creates a new directory. The parent directory must exist. Initializes `.` and `..` entries. Returns `-1` on failure.

### `vfs_unlink`

```c
int vfs_unlink(const char *path);
```

Deletes a regular file. Frees the cluster chain and marks the directory entry deleted (`name[0] = 0xE5`). Returns `-1` if path is a directory or does not exist.

### `vfs_rmdir`

```c
int vfs_rmdir(const char *path);
```

Deletes an empty directory. Fails if the directory contains any entries other than `.` and `..`. Returns `-1` on failure.

### `vfs_rename`

```c
int vfs_rename(const char *old_path, const char *new_path);
```

Renames or moves a file or directory. The destination name must not already exist. The parent directory of the destination must exist. Works across directories. Returns `-1` on failure.

### `vfs_readdir`

```c
int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

Iterates all non-deleted, non-LFN, non-volume-label entries in the directory at `path`. Calls `cb` with a `vfs_dirent_t` for each entry. Stops if `cb` returns non-zero. Returns `0` on success, `-1` if `path` is not a directory or does not exist.

### `vfs_last_error`

```c
const char *vfs_last_error(void);
```

Returns a static string describing the last VFS error. Valid until the next VFS call. Never returns NULL (returns `""` if no error has occurred).

### `vfs_normalize`

```c
void vfs_normalize(char *out, const char *cwd, const char *path);
```

Resolves `path` into an absolute path written to `out` (max `PUREUNIX_MAX_PATH` bytes):

- If `path` begins with `/`, it is treated as absolute; `cwd` is ignored.
- Otherwise, `path` is appended to `cwd`.
- `.` components are skipped.
- `..` components pop the last path segment; clamped at root.
