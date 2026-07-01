# Filesystem

## Overview

The filesystem layer has three components:

- **EXT2 driver** (`fs/ext2/`) — read-only driver for the ATA primary slave disk. Handles superblock parsing, block group descriptor table, inode reads, directory traversal, direct and singly-indirect block iteration, and a 4-slot LRU block cache.
- **FAT16 driver** (`fs/fat16.c`) — read/write driver for the ATA primary master disk. Handles cluster allocation/freeing, file and directory CRUD, and all write operations.
- **VFS** (`fs/vfs.c`) — dual-dispatch layer over both drivers. Read operations (stat, read, readdir) try EXT2 first and merge FAT16 results. Write operations route exclusively to FAT16.

The two filesystems partition the namespace by disk:

| Disk | Role | Filesystem |
|---|---|---|
| ATA primary master (`index=0`) | Program store | FAT16 (read/write) |
| ATA primary slave (`index=1`) | Data filesystem | EXT2 (read-only) |

---

## VFS

**Source**: `fs/vfs.c`
**Header**: `include/pureunix/vfs.h`

### Dispatch Policy

| Operation | EXT2 | FAT16 |
|---|---|---|
| `vfs_stat` | tried first; returns if found | tried if EXT2 fails |
| `vfs_read_file` | tried first; returns if found | tried if EXT2 fails |
| `vfs_readdir` | called if path is a directory in EXT2 | always called if path is a directory in FAT16; results are merged |
| `vfs_write_file`, `vfs_create`, `vfs_mkdir`, `vfs_unlink`, `vfs_rmdir`, `vfs_rename` | not used (EXT2 is read-only) | exclusive |

`vfs_readdir` calls both drivers when both have the directory. This makes `ls /` show entries from both the EXT2 data partition (README.TXT, etc/) and the FAT16 program store (/bin). Entries from the two sources are interleaved in callback order: all EXT2 entries first, then all FAT16 entries.

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
    uint16_t        mode;   // FAT attribute byte (0 for EXT2 entries)
} vfs_stat_t;

typedef struct vfs_dirent {
    char            name[PUREUNIX_MAX_NAME];  // 64 bytes
    vfs_node_type_t type;
    uint32_t        size;
} vfs_dirent_t;

typedef int (*vfs_readdir_cb_t)(const vfs_dirent_t *entry, void *ctx);
```

`vfs_readdir` calls `cb` for each visible entry from any mounted filesystem. Return non-zero from `cb` to stop iteration.

---

## EXT2 Driver

**Sources**: `fs/ext2/super.c`, `fs/ext2/block.c`, `fs/ext2/inode.c`, `fs/ext2/dir.c`, `fs/ext2/file.c`, `fs/ext2/mount.c`
**Internal headers**: `fs/ext2/*.h`
**Public header**: `include/pureunix/ext2.h`

### On-Disk Layout

```
Block 0:            Boot block (unused)
Block 1:            Superblock (at byte offset 1024 from filesystem start = LBA 2–3)
Block 2:            Block Group Descriptor Table (BGDT)
Block 3:            Block bitmap
Block 4:            Inode bitmap
Blocks 5–132:       Inode table (128 blocks for 1024 inodes at 128 bytes each)
Blocks 133+:        Data blocks
```

The EXT2 image is 4 MiB with 1 KiB blocks, 1 block group, and 1024 inodes.

### Superblock

Read by `ext2_super_read(disk)` at mount time. Key fields extracted:

| Field | Source |
|---|---|
| `block_size` | `1024 << s_log_block_size` |
| `inode_size` | `s_inode_size` (dynamic rev) or 128 (rev 0) |
| `inodes_per_group` | `s_inodes_per_group` |
| `blocks_per_group` | `s_blocks_per_group` |
| `num_groups` | computed from `s_blocks_count / s_blocks_per_group` |
| `first_data_block` | `s_first_data_block` |

Magic is validated (`0xEF53`). Unsupported `s_feature_incompat` bits cause mount failure.

### Block Group Descriptor Table

Allocated as `bgdt_blocks * block_size` bytes (rounded up to whole blocks). The allocation must be at least one full block because the sector-read loop writes `block_size` bytes per block regardless of how many descriptors are in use. Allocating only `num_groups * 32` bytes (for a single group) causes a heap buffer overflow.

Each 32-byte `ext2_bgdt_entry_t` provides:

```c
uint32_t bg_block_bitmap;   // block number of block bitmap
uint32_t bg_inode_bitmap;   // block number of inode bitmap
uint32_t bg_inode_table;    // first block of inode table
uint16_t bg_free_blocks;
uint16_t bg_free_inodes;
uint16_t bg_used_dirs;
```

### Block Cache

`fs/ext2/block.c` implements a 4-slot LRU block cache in BSS (16 KiB static allocation).

```c
const uint8_t *ext2_read_block(uint32_t blk_no);   // non-owning; do NOT kfree
void           ext2_block_cache_flush(void);        // invalidate all slots
```

Callers receive a non-owning pointer into the cache. The pointer is valid until the next `ext2_read_block` call that evicts the slot (after all 4 slots are occupied). Code that calls `ext2_read_block` inside a loop must copy any data needed across iterations before the next call.

The cache eliminates `kmalloc`/`kfree` per block read entirely; block reads are allocation-free.

### Inode

`ext2_read_inode(ino, out)` in `fs/ext2/inode.c`:

1. Computes `group = (ino - 1) / inodes_per_group` and `local_idx = (ino - 1) % inodes_per_group`.
2. Reads the inode table block via `ext2_read_block`.
3. Copies `EXT2_MIN_INODE_SIZE` (128) bytes at the correct offset into `out`.

Key inode fields:

```c
typedef struct {
    uint16_t i_mode;        // file type + permissions (EXT2_S_IFREG / EXT2_S_IFDIR)
    uint16_t i_uid;
    uint32_t i_size;        // file size in bytes
    // ... timestamps ...
    uint32_t i_block[15];   // block pointers: [0–11] direct, [12] singly-indirect
} ext2_inode_t;
```

### Block Iteration

`ext2_iter_blocks(inode, nbytes, cb, ctx)` visits each data block number in order:

- **Direct blocks** [0–11]: called directly.
- **Singly-indirect block** [12]: the indirect block's pointer array is copied into a local `uint32_t local_ptrs[256]` array *before* calling any callback. This prevents cache eviction of the indirect block during callback execution from producing stale reads.
- Doubly-indirect and triply-indirect blocks are not implemented.

### Directory

`ext2_path_to_inode(path, out_ino)` in `fs/ext2/dir.c` walks the directory tree from root inode (inode 2), splitting the path on `/` and calling `ext2_dir_lookup` at each step.

`ext2_readdir_ino(dir_ino, cb, ctx)` enumerates all non-`.`/`..` entries in a directory. For each file entry it reads the file's inode to obtain `i_size`. The inode table block is typically in the block cache after the first read, making this cheap.

Directory entry structure:

```c
typedef struct {
    uint32_t inode;         // 0 = deleted/unused
    uint16_t rec_len;       // byte length of this entry (advances to next)
    uint8_t  name_len;
    uint8_t  file_type;     // EXT2_FT_REG_FILE or EXT2_FT_DIR
    char     name[];        // not null-terminated; length given by name_len
} ext2_dirent_t;
```

### Public API

```c
int  ext2_mount(disk_device_t *disk);
void ext2_unmount(void);
bool ext2_is_mounted(void);

int  ext2_stat(const char *path, vfs_stat_t *st);
int  ext2_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int  ext2_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

`ext2_read_file` allocates a `kcalloc`'d buffer for the entire file. The caller must `kfree` it.

### EXT2 Disk Image

`tools/mkext2.py` creates `build/ext2.img` (4 MiB):

| Parameter | Value |
|---|---|
| Block size | 1024 bytes |
| Total blocks | 4096 |
| Block groups | 1 |
| Inodes | 1024 |

Contents:

```
/README.TXT             207 bytes — informational text
/etc/
    passwd              user account list
    hostname            hostname string
/home/
    user/
        notes.txt       sample user notes
/testdir/
    alpha.txt
    beta.txt
    gamma.txt
/bigfile.bin            5120 bytes (5 × 1 KB blocks, all 'B')
/hugefile.bin           14336 bytes (14 × 1 KB blocks, bytes 0–255 repeating)
```

`bigfile.bin` tests block-boundary reads (spans blocks 1–5). `hugefile.bin` tests singly-indirect block reads (exceeds 12 direct blocks; blocks 13–14 are via `i_block[12]`).

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

Only a single FAT16 filesystem is supported (global static `fs`).

### Mount

`fat16_mount(disk)`:

1. Reads sector 0.
2. Verifies boot signature `0x55 0xAA` at bytes 510–511.
3. Validates `bytes_per_sector == 512` and `sectors_per_fat16 != 0`.
4. Populates `fs` from the BPB fields.
5. Sets `fs.mounted = true`.

Returns `0` on success, `-1` on failure.

### FAT Entries

| Value | Meaning |
|---|---|
| `0x0000` | Free cluster |
| `0x0002–0xFFF6` | Next cluster in chain |
| `0xFFF8–0xFFFF` | End-of-chain (EOF) |

`fat_read(cluster, &value)` reads from the first FAT copy.
`fat_write(cluster, value)` writes to **all** FAT copies atomically.

### 8.3 Filename Encoding

Filenames are stored as 11 bytes: 8 for the base, 3 for the extension, space-padded.

`name_to_83(name, out[11])`: converts to uppercase padded format. Returns `-1` for invalid names.

`name_from_83(entry, out)`: converts stored bytes back to a dotted lowercase string.

Long File Name (LFN) entries (attribute `0x0F`) are **skipped** during directory scans.

### Directory Entry

```c
typedef struct fat_dir_entry {
    uint8_t  name[11];             // 8.3 padded uppercase
    uint8_t  attr;
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
FAT_ATTR_LFN        0x0F
```

Free entries: `name[0] == 0x00` (never used) or `name[0] == 0xE5` (deleted).

### Path Lookup

`path_lookup(path, out)` resolves an absolute path by tokenizing on `/` and scanning directories linearly.

### File Read / Write / Create / Delete / Rename

See the internal implementation in `fs/fat16.c`. All write operations: `fat16_write_file`, `fat16_create`, `fat16_unlink`, `fat16_rename`.

### Directory Listing

`fat16_readdir(path, cb, ctx)`: iterates entries, skipping deleted, LFN, and volume ID entries. Calls `cb` with `vfs_dirent_t`.

### Space Reporting

```c
uint32_t fat16_free_bytes(void);
uint32_t fat16_total_bytes(void);
```

Both are O(n) in cluster count.

---

## FAT16 Disk Image

`tools/mkfat16.py` creates `build/pureunix.img` (32 MiB):

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
/README.TXT         informational text
/BIN/
    CALC.ELF
    HELLO.ELF
    VIEWER.ELF
    EDITOR.ELF
    SH.ELF
    OPENTEST.ELF
    READTEST.ELF
    EXT2TEST.ELF
/DOCS/              documentation files from docs/
```

The Makefile invokes it as:
```
python3 tools/mkfat16.py build/pureunix.img --docs docs build/user/*.elf
```

---

## File Descriptor Table

Each `task_t` carries a `fd_entry_t fds[MAX_OPEN_FILES]` array (16 slots). This table bridges user-space file descriptors and the VFS.

```c
#define MAX_OPEN_FILES 16

typedef struct fd_entry {
    bool     used;
    char     path[PUREUNIX_MAX_PATH];
    uint8_t *data;    /* entire file contents, kmalloc'd at open() time */
    size_t   size;
    size_t   offset;  /* current read/seek position */
    int      flags;   /* open() flags: O_RDONLY / O_WRONLY / O_RDWR */
} fd_entry_t;
```

Slots 0, 1, 2 are permanently reserved for stdin/stdout/stderr. `SYS_OPEN` allocates from slots 3–15.

**Descriptor lifecycle**:

1. `SYS_OPEN` calls `vfs_read_file()` (EXT2-first, then FAT16), allocates a `kmalloc`'d buffer for the entire file, stores it in `fd_entry_t.data`, and sets `offset = 0`.
2. `SYS_READ` with fd ≥ 3 copies bytes from `fd_entry_t.data` at `offset`, advances `offset`, and returns the byte count. Returns 0 at EOF.
3. `SYS_LSEEK` updates `fd_entry_t.offset` without touching `data`.
4. `SYS_CLOSE` calls `kfree(fd_entry_t.data)` and zeroes the slot.

**`struct stat` / `SYS_STAT`**:

`SYS_STAT` calls `vfs_stat()` (EXT2-first, FAT16-fallback) and populates:

```c
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = VFS_FILE, 2 = VFS_DIR */
    unsigned short st_attr;   /* FAT attribute byte (0 for EXT2 entries) */
};
```

**Seek semantics**:

| whence | Formula |
|---|---|
| `SEEK_SET` (0) | `new_offset = offset` |
| `SEEK_CUR` (1) | `new_offset = fd.offset + offset` |
| `SEEK_END` (2) | `new_offset = fd.size + offset` |

A negative resulting offset returns `-EINVAL`. Seeking past the end of the file is permitted.

---

## Limitations

- EXT2 is read-only; no write or create operations.
- EXT2 does not support doubly- or triply-indirect blocks.
- FAT16 filenames restricted to 8.3; no LFN support.
- Single FAT16 and single EXT2 instance; no unmount.
- No file permissions beyond FAT attribute bytes (EXT2 mode bits are not surfaced to userland).
- No timestamps surfaced to userland.
- Root directory has a fixed 512-entry capacity in FAT16.
- `fat16_rename` does not support true atomic cross-directory moves.
