# Filesystem

## Overview

The filesystem layer has three components:

- **EXT2 driver** (`fs/ext2/`) — read/write driver for the ATA primary slave disk (Stage 4 made it writable; see `fs/ext2/alloc.c`/`fs/ext2/write.c`). Handles superblock parsing, block group descriptor table, inode reads/writes, block and inode allocation/freeing, directory traversal/insertion/removal, direct and singly-indirect block iteration, symbolic and hard links, and a 4-slot LRU block cache. Primary root filesystem, mounted at `/`.
- **FAT16 driver** (`fs/fat16.c`) — read/write driver for the ATA primary master disk. Handles cluster allocation/freeing, file and directory CRUD, and all write operations. Mounted at `/fat` for compatibility/testing only. Unlike EXT2, it has no concept of symlinks or hard links.
- **VFS** (`fs/vfs.c`) — a mount-table router, *not* a dual-dispatch/union layer: every path resolves via longest-prefix match to exactly one mount, and that mount's driver alone serves the call (see `docs/api/vfs.md` for the full mount-table API). Stage 3A added Unix permission enforcement at this same layer; Stage 4 added the centralized `resolve_path()` pathname resolver (symlink-following, `.`/`..`, loop detection) that every `vfs_*` entry point is now built on.

| Disk | Role | Filesystem | Mountpoint |
|---|---|---|---|
| ATA primary master (`index=0`) | Compatibility/testing store | FAT16 (read/write) | `/fat` |
| ATA primary slave (`index=1`) | Primary root filesystem | EXT2 (read/write) | `/` |

---

## VFS

**Source**: `fs/vfs.c`
**Header**: `include/pureunix/vfs.h`
**Full API reference**: `docs/api/vfs.md`

### Dispatch

Every `vfs_*` call resolves `path` to a single mount (longest-prefix match against the mount table registered via `vfs_mount()` in `kernel_main`) and calls straight into that mount's `vfs_ops_t`. `/etc`, `/bin`, `/docs`, etc. all live under the EXT2 mount at `/`; `/fat/...` resolves to the FAT16 mount, with the `/fat` prefix stripped before the driver sees the path. There is no merging — `ls /` shows only EXT2's root, `ls /fat` shows only FAT16's root.

### Permissions (Stage 3A)

Every `vfs_*` call also runs the calling task's credentials (`current_uid()`/`current_gid()`) through the central permission engine, `vfs_access()`, before touching the driver:

- Ancestor directories in `path` need `X_OK` (search permission) — enforced for every call, read or write.
- `vfs_read_file`/`vfs_readdir` additionally require `R_OK` on the target.
- The write-side calls (`vfs_write_file`, `vfs_create`, `vfs_mkdir`, `vfs_unlink`, `vfs_rmdir`, `vfs_rename`, `vfs_link`, `vfs_symlink`) require `W_OK` on whichever directory/file the operation modifies — enforced unconditionally, so any mounted filesystem gets enforcement for free, regardless of whether its `vfs_ops_t` actually implements the operation (a driver that doesn't leaves the slot `NULL` and the VFS reports `-EROFS`).
- uid 0 (root) always gets read/write; execute additionally requires at least one execute bit set somewhere on the file.

Neither driver knows anything about permissions — `vfs_access()` operates purely on the `vfs_stat_t` metadata drivers already hand back. See `docs/api/vfs.md`'s "Permissions" section for the full model, and `include/pureunix/task.h` for process credentials.

### API

```c
int  vfs_init(void);
bool vfs_mounted(void);
int  vfs_mount_root(void);

int  vfs_stat(const char *path, vfs_stat_t *st);
int  vfs_lstat(const char *path, vfs_stat_t *st);
int  vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int  vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int  vfs_create(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *old_path, const char *new_path);
int  vfs_link(const char *old_path, const char *new_path);
int  vfs_symlink(const char *target, const char *path);
int  vfs_readlink(const char *path, char *buf, size_t bufsize);
int  vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
const char *vfs_last_error(void);
void vfs_normalize(char *out, const char *cwd, const char *path);
```

All path arguments must be absolute (begin with `/`). Every function returns `0` on success and a negative errno on failure (uniform since Stage 4 — see `docs/api/vfs.md` for the full list of codes and the `resolve_path()` pathname resolver every entry point above is built on, including symlink-following, `.`/`..`, and `-ELOOP` loop detection).

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

See `docs/api/vfs.md` for the full, current `vfs_stat_t` (Unix metadata: mode, uid, gid, nlink, inode number, timestamps, block count/size — direct from the inode on EXT2, synthesized on FAT16) and `vfs_dirent_t` definitions, and `vfs_node_type_t`'s three values (`VFS_FILE`, `VFS_DIR`, `VFS_SYMLINK`).

`vfs_readdir` calls `cb` for each visible entry from any mounted filesystem. Return non-zero from `cb` to stop iteration.

---

## EXT2 Driver

**Sources**: `fs/ext2/super.c`, `fs/ext2/block.c`, `fs/ext2/alloc.c`, `fs/ext2/inode.c`, `fs/ext2/dir.c`, `fs/ext2/file.c`, `fs/ext2/write.c`, `fs/ext2/mount.c`
**Internal headers**: `fs/ext2/*.h`
**Public header**: `include/pureunix/ext2.h`

`fs/ext2/alloc.c` and `fs/ext2/write.c` (Stage 4) hold the write path: block/inode bitmap allocation and freeing, directory entry insertion/removal (including growing a directory by allocating additional blocks as it fills up), and `create`/`unlink`/`rename`/`link`/`symlink`/`readlink`/`write_file`.

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

`ext2_readdir_ino(dir_ino, cb, ctx)` enumerates **every** entry in a directory, including `.` and `..` — they are real on-disk directory entries like any other, and the driver never filters them (it's the shell's job to hide them by default and show them under `ls -a`). For each non-directory entry it also reads the file's inode to obtain `i_size`; that nested `ext2_read_inode()` call reads a *different* block (the inode table) than the directory block currently being scanned, so the directory block is copied into a local stack buffer before scanning starts — holding a live cache pointer across that nested read let the directory-block cache slot get evicted and silently swapped out mid-scan, truncating large directory listings.

`ext2_dir_insert(dir_ino, name, ino, file_type)` adds one entry: it first tries to reuse a deleted slot or split an existing entry's trailing slack space in an already-allocated block, and only allocates and appends a fresh block once no existing block has room. `ext2_dir_remove(dir_ino, name)` merges a removed entry's `rec_len` into its predecessor (or, if first in its block, just clears `inode` to mark the slot reusable) — directory blocks are never compacted across entries this way, only within a block's own free space.

Directory entry structure:

```c
typedef struct {
    uint32_t inode;         // 0 = deleted/unused
    uint16_t rec_len;       // byte length of this entry (advances to next)
    uint8_t  name_len;
    uint8_t  file_type;     // EXT2_FT_REG_FILE, EXT2_FT_DIR, or EXT2_FT_SYMLINK
    char     name[];        // not null-terminated; length given by name_len
} ext2_dirent_t;
```

### Public API

```c
int  ext2_mount(disk_device_t *disk);
bool ext2_is_mounted(void);

int  ext2_stat(const char *path, vfs_stat_t *st);
int  ext2_read_file(const char *path, uint8_t **out_data, size_t *out_size);
int  ext2_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags);
int  ext2_create(const char *path, bool directory);
int  ext2_unlink(const char *path, bool directory);
int  ext2_rename(const char *old_path, const char *new_path);
int  ext2_link(const char *old_path, const char *new_path);
int  ext2_symlink(const char *target, const char *path);
int  ext2_readlink(const char *path, char *buf, size_t bufsize);
int  ext2_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx);
```

`ext2_read_file` allocates a `kcalloc`'d buffer for the entire file. The caller must `kfree` it. `ext2_write_file` frees the inode's existing blocks and writes the new content across freshly-allocated blocks (append is handled by the caller pre-combining old + new content, not by the driver).

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
/README.TXT             informational text
/etc/
    passwd              user account list
    hostname            hostname string
/bin/                   ELF program store, mode 0755 (rwxr-xr-x) — see Permissions below
    *.elf
/docs/                  documentation tree, mirrored from docs/*.md
    api/
/home/
    user/
        notes.txt       sample user notes
/testdir/
    alpha.txt
    beta.txt
    gamma.txt
    uplink              symlink -> ../README.TXT (relative, resolved from testdir's own parent)
/perm/                  Stage 3A permission-engine regression fixtures
    exec.sh             mode 0755, uid=0, gid=0
    private.txt         mode 0600, uid=0, gid=0 — owner-only
    readonly.txt        mode 0444, uid=0, gid=0 — world-readable, root-writable only
    noaccess.bin         mode 0000, uid=0, gid=0 — nobody but root (and root needs no x bit exemption for r/w)
    group_test.txt       mode 0640, uid=0, gid=100 — exercises the group tier specifically
    emptydir/            directory with nothing but '.' and '..'
/readme.link            symlink -> README.TXT (ext2 "fast symlink", inline target, no data block)
/abslink                symlink -> /README.TXT (absolute-target fast symlink)
/loop_a                 symlink -> loop_b (Stage 4 ELOOP regression fixture)
/loop_b                 symlink -> loop_a (cycle with loop_a; resolution must give up after 40 hops)
/bin/hello              symlink -> hello.elf (exercises exec-through-a-symlink)
/bigfile.bin            5120 bytes (5 × 1 KB blocks, all 'B')
/hugefile.bin           14336 bytes (14 × 1 KB blocks, bytes 0–255 repeating)
```

`bigfile.bin` tests block-boundary reads (spans blocks 1–5). `hugefile.bin` tests singly-indirect block reads (exceeds 12 direct blocks; blocks 13–14 are via `i_block[12]`). Every inode gets a real (image build time) timestamp — see `tools/mkext2.py`'s `BUILD_TIME` — rather than the all-zero placeholder used before Stage 2D.

The rest of the writable-filesystem surface (files/directories/hard links created and removed at runtime — `mkdir`, `creat`/`write`, `unlink`, `rmdir`, `rename`, `link`, `symlink` with both fast and block-based targets) is exercised by `user/ext2test.c`'s regression suite rather than baked into the image; see that file for the full scenario list.

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

`SYS_STAT` resolves `path` through the VFS mount table and populates a `struct stat` with size/type plus full Unix metadata (mode, uid, gid, nlink, inode number, timestamps, block count/size) and enforces `X_OK` on every ancestor directory along the way. See `docs/syscalls.md`'s `SYS_STAT` section for the current layout, and `docs/api/vfs.md` for the permission model.

**Seek semantics**:

| whence | Formula |
|---|---|
| `SEEK_SET` (0) | `new_offset = offset` |
| `SEEK_CUR` (1) | `new_offset = fd.offset + offset` |
| `SEEK_END` (2) | `new_offset = fd.size + offset` |

A negative resulting offset returns `-EINVAL`. Seeking past the end of the file is permitted.

---

## Limitations

- EXT2 does not support doubly- or triply-indirect blocks (files are capped at 12 + 256 = 268 blocks, i.e. ~268 KiB with 1 KiB blocks).
- FAT16 filenames restricted to 8.3; no LFN support; FAT16 has no concept of symlinks or hard links (its `vfs_ops_t` leaves `readlink`/`link`/`symlink` `NULL`).
- Single FAT16 and single EXT2 instance; no unmount.
- Root directory has a fixed 512-entry capacity in FAT16.
- `fat16_rename` does not support true atomic cross-directory moves.
- Permissions are enforced (Stage 3A — see `docs/api/vfs.md`), but there is no
  ACL/capability model, no setuid/setgid, no sticky bit, and no groups
  database. `chmod`/`chown` exist only as `-EROFS` syscall stubs on both
  filesystems; neither can persist a permission or ownership change yet.
- No login system: every process is uid 0 (root) in practice. Credentials
  exist and are enforced, but nothing can set them to anything else outside
  the `ext2test` regression suite's test-only `SYS_DEBUG_SETCRED` hook.
- No `O_RDWR`: a file opened for reading can't also be written, and vice
  versa; read-modify-write requires a full read followed by a separate
  truncating write.
