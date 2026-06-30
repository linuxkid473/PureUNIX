# Filesystem

## Overview

The filesystem layer has two components:

- **FAT16 driver** (`fs/fat16.c`) — directly reads and writes the disk using the ATA driver.
- **VFS** (`fs/vfs.c`) — a thin dispatch layer that calls into the FAT16 driver and provides path normalization.

There is no plugin mechanism or multiple-filesystem support. The VFS is effectively an alias for FAT16.

---

## VFS

**Source**: `fs/vfs.c`  
**Header**: `include/pureunix/vfs.h`

### Purpose

The VFS layer decouples the rest of the kernel from the FAT16 implementation. Every VFS function calls the corresponding `fat16_*` function and, on failure, records an error string in a static `last_error` buffer.

### API

```c
int  vfs_init(void);
bool vfs_mounted(void);
int  vfs_mount_root(void);

int  vfs_stat(const char *path, vfs_stat_t *st);
int  vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int  vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int  vfs_create(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *old_path, const char *new_path);
int  vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
const char *vfs_last_error(void);
void vfs_normalize(char *out, const char *cwd, const char *path);
```

All path arguments must be absolute (begin with `/`). Return value is `0` on success, `-1` on failure.

### Write Flags

```c
#define VFS_O_APPEND 0x01   // append data to existing file content
#define VFS_O_TRUNC  0x02   // truncate and replace existing content
```

### Path Normalization

`vfs_normalize(out, cwd, path)` resolves a path against the current working directory:

- If `path` starts with `/`, it is absolute; `cwd` is ignored.
- Otherwise, `path` is resolved relative to `cwd`.
- `.` components are skipped.
- `..` components pop the last path segment (clamped at root).
- Result is written to `out` (max `PUREUNIX_MAX_PATH` = 256 bytes).

### Types

```c
typedef enum { VFS_FILE = 1, VFS_DIR = 2 } vfs_node_type_t;

typedef struct vfs_stat {
    vfs_node_type_t type;
    uint32_t        size;   // file size in bytes; 0 for directories
    uint16_t        mode;   // FAT attribute byte
} vfs_stat_t;

typedef struct vfs_dirent {
    char            name[PUREUNIX_MAX_NAME];  // 64 bytes
    vfs_node_type_t type;
    uint32_t        size;
} vfs_dirent_t;

typedef int (*vfs_readdir_cb_t)(const vfs_dirent_t *entry, void *ctx);
```

`vfs_readdir` calls `cb` for each visible entry. Return non-zero from `cb` to stop iteration.

---

## FAT16 Driver

**Source**: `fs/fat16.c`  
**Header**: `include/pureunix/fat16.h`

### Filesystem Layout

```
Sector 0:                Boot sector / BPB
Sectors 1 – (first_fat_sector + fat_count*sectors_per_fat - 1):
                         FAT tables (two copies, kept in sync on write)
Root directory area:     Fixed-size array of 512 32-byte entries
Data region:             Clusters starting at cluster number 2
```

### Internal State

```c
typedef struct fat16_fs {
    disk_device_t *disk;
    bool           mounted;
    uint16_t       bytes_per_sector;    // must be 512
    uint8_t        sectors_per_cluster;
    uint16_t       reserved_sectors;
    uint8_t        fat_count;           // typically 2
    uint16_t       root_entry_count;    // 512 in the generated image
    uint16_t       sectors_per_fat;
    uint32_t       total_sectors;
    uint32_t       root_dir_sectors;
    uint32_t       first_fat_sector;
    uint32_t       root_dir_sector;
    uint32_t       first_data_sector;
    uint32_t       max_cluster;
} fat16_fs_t;
```

Only a single filesystem is supported (global static `fs`).

### Mount

`fat16_mount(disk)`:

1. Reads sector 0.
2. Verifies boot signature `0x55 0xAA` at bytes 510–511.
3. Validates `bytes_per_sector == 512` and `sectors_per_fat16 != 0`.
4. Populates `fs` from the BPB fields.
5. Sets `fs.mounted = true`.

Returns `0` on success, `-1` on failure (no disk, bad signature, invalid geometry).

### FAT Entries

The FAT is a flat array of 16-bit little-endian cluster values.

| Value | Meaning |
|---|---|
| `0x0000` | Free cluster |
| `0x0002–0xFFF6` | Next cluster in chain |
| `0xFFF8–0xFFFF` | End-of-chain (EOF) |

`fat_read(cluster, &value)` reads from the first FAT copy.  
`fat_write(cluster, value)` writes to **all** FAT copies atomically (read–modify–write per sector).

### 8.3 Filename Encoding

Filenames are stored as 11 bytes: 8 for the base, 3 for the extension, space-padded.

`name_to_83(name, out[11])`: converts to uppercase padded format. Returns `-1` for invalid names.

`name_from_83(entry, out)`: converts stored bytes back to a dotted lowercase string.

Long File Name (LFN) entries (attribute `0x0F`) are **skipped** during directory scans. LFN names are not read or written.

### Directory Entry

```c
typedef struct fat_dir_entry {
    uint8_t  name[11];             // 8.3 padded uppercase
    uint8_t  attr;                 // FAT attribute flags
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;   // always 0 in FAT16
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;
```

Attribute flags:

```c
FAT_ATTR_READ_ONLY  0x01
FAT_ATTR_HIDDEN     0x02
FAT_ATTR_SYSTEM     0x04
FAT_ATTR_VOLUME_ID  0x08
FAT_ATTR_DIRECTORY  0x10
FAT_ATTR_ARCHIVE    0x20
FAT_ATTR_LFN        0x0F   // all four low bits set = LFN entry (skipped)
```

Free entries: `name[0] == 0x00` (never used) or `name[0] == 0xE5` (deleted).

### Path Lookup

`path_lookup(path, out)` resolves an absolute path:

1. Validates `fs.mounted` and `path[0] == '/'`.
2. Returns a synthetic entry for `/`.
3. Tokenizes the remaining path on `/` with `strtok_r`.
4. For each component, calls `find_in_dir` on the current directory cluster (cluster 0 = root).
5. Intermediate components must be directories; returns `-1` if not.
6. Fills `out` with `found_entry_t` (entry, parent cluster, index in parent).

`find_in_dir(dir_cluster, fat_name[11], out)` scans entries linearly, comparing the full 11-byte name.

### File Read

`fat16_read_file(path, &data, &size)`:

1. `path_lookup`; rejects directories.
2. Allocates `entry.size + 1` bytes (`kmalloc`).
3. Follows cluster chain, reads sectors into stack buffer, copies to output.
4. Appends a null terminator.
5. Caller owns the buffer; must call `kfree`.

### File Write

`fat16_write_file(path, data, size, flags)`:

1. If file does not exist, calls `fat16_create` first.
2. If `VFS_O_APPEND`: reads old content, concatenates, writes combined data.
3. Calls `write_new_chain` to allocate clusters and write data.
4. Calls `free_chain` to release the old cluster chain.
5. Updates the directory entry (new first cluster, new size, attribute = `ARCHIVE`).

`alloc_cluster`: linear scan from cluster 2 for `FAT_FREE`, marks as end-of-chain, zeroes all sectors.

### File Create

`fat16_create(path, directory)`:

1. Resolves parent directory and converts leaf to 8.3.
2. Checks for duplicate name in parent.
3. Finds a free slot (extends directory with new cluster if needed).
4. For directories: allocates one cluster, writes `.` and `..` entries.
5. Writes the new 32-byte directory entry.

### File Delete

`fat16_unlink(path, directory)`:

1. Resolves path.
2. For directories: verifies empty (no visible entries except `.` and `..`).
3. Frees cluster chain.
4. Marks entry deleted (`name[0] = 0xE5`).

### Rename

`fat16_rename(old_path, new_path)`:

1. Resolves old entry and new parent.
2. Checks new name does not already exist.
3. Finds a free slot in new parent directory.
4. Writes the moved entry under the new name.
5. Marks old entry deleted.

**Limitation**: moving across directory boundaries copies the directory entry but the cluster chain stays in place, which is correct behavior. However, if the source and destination are in different directories, this works as an atomic rename only if the parent cluster is different.

### Directory Listing

`fat16_readdir(path, cb, ctx)`: iterates all entries in a directory, skipping deleted, LFN, and volume ID entries. Calls `cb` with a `vfs_dirent_t`. Returns `0` on success.

### Space Reporting

```c
uint32_t fat16_free_bytes(void);    // scans entire FAT for free clusters
uint32_t fat16_total_bytes(void);   // (max_cluster - 2) * cluster_size
```

Both are O(n) in cluster count.

---

## Disk Image

`tools/mkfat16.py` creates `build/pureunix.img`:

| Parameter | Value |
|---|---|
| Total size | 32 MiB (65536 × 512 bytes) |
| Sectors per cluster | 4 (2 KiB clusters) |
| FAT copies | 2 |
| Root entries | 512 |
| OEM string | `PUREUNIX` |
| Volume label | `PUREUNIX   ` |

Contents:

```
/README.TXT     informational text
/BIN/
    CALC.ELF
    HELLO.ELF
    VIEWER.ELF
    EDITOR.ELF
    SH.ELF
```

The Makefile invokes it as:
```
python3 tools/mkfat16.py build/pureunix.img build/user/*.elf
```

---

## File Descriptor Table

Each `task_t` carries a `fd_entry_t fds[MAX_OPEN_FILES]` array (16 slots). This table is the bridge between user-space file descriptors and the VFS.

```c
#define MAX_OPEN_FILES 16

typedef struct fd_entry {
    bool     used;
    char     path[PUREUNIX_MAX_PATH];
    uint8_t *data;    /* entire file contents, kmalloc'd at open() time */
    size_t   size;
    size_t   offset;  /* current read/seek position */
} fd_entry_t;
```

Slots 0, 1, 2 are permanently reserved for stdin/stdout/stderr and are marked `used = true` during `tasking_init` and `task_create`. `SYS_OPEN` allocates from slots 3–15.

**Descriptor lifecycle**:

1. `SYS_OPEN` calls `vfs_read_file()`, allocates a `kmalloc`'d buffer for the entire file, stores it in `fd_entry_t.data`, and sets `offset = 0`.
2. `SYS_LSEEK` updates `fd_entry_t.offset` without touching `data`. The offset may be set past the end of the file; no data is read or written.
3. `SYS_CLOSE` calls `kfree(fd_entry_t.data)` and zeroes the entire `fd_entry_t`, making the slot available for reuse by a subsequent `SYS_OPEN`. Descriptors 0, 1, 2 cannot be closed.

**`struct stat` / `SYS_STAT`**:

`SYS_STAT` calls `vfs_stat()` (which calls `fat16_stat()` → `path_lookup()`) and populates a user-provided buffer:

```c
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = VFS_FILE, 2 = VFS_DIR */
    unsigned short st_attr;   /* FAT attribute byte (FAT_ATTR_* flags) */
};
```

`stat()` works on any path — it does not require the file to be open. It resolves the path through the FAT16 directory tree and returns metadata directly from the directory entry. No open descriptor is consumed.

**Seek semantics**:

`SYS_LSEEK` supports three modes:

| whence | Formula |
|---|---|
| `SEEK_SET` (0) | `new_offset = offset` |
| `SEEK_CUR` (1) | `new_offset = fd.offset + offset` |
| `SEEK_END` (2) | `new_offset = fd.size + offset` |

A negative resulting offset returns `-EINVAL`. Seeking past the end of the file is permitted. The new offset is returned on success.

---

## Limitations

- 8.3 filenames only; no LFN support.
- Single mounted filesystem; no unmount.
- No file permissions beyond FAT attribute bytes.
- No timestamps (create/write times are set to zero).
- `fat16_rename` does not support cross-directory moves atomically.
- Root directory has a fixed 512-entry capacity.
- No bad-cluster detection or handling.
- `SYS_READ` does not yet handle fd ≥ 3; reading from an opened file via syscall is not yet implemented.
