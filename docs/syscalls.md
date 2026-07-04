# System Calls

## Overview

User-space programs invoke the kernel via `int $0x80`. The syscall number is passed in `EAX`; up to three arguments in `EBX`, `ECX`, `EDX`. The return value is written back to `EAX`.

The dispatch function is `syscall_dispatch` in `arch/i386/syscall.c`. It is called by `isr_dispatch` when vector 0x80 fires.

---

## Calling Convention

| Register | Role |
|---|---|
| `EAX` | Syscall number (input); return value (output) |
| `EBX` | Argument 1 |
| `ECX` | Argument 2 |
| `EDX` | Argument 3 |

The libpure helper `syscall3` in `user/libpure.c` implements this convention:

```c
static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80"
        : "=a"(r)
        : "a"(n), "b"(a), "c"(b), "d"(c)
        : "memory");
    return r;
}
```

---

## Syscall Table

| Number | Name | EBX | ECX | EDX | Return |
|---|---|---|---|---|---|
| 1 | `SYS_EXIT` | exit code | — | — | EBX value |
| 2 | `SYS_WRITE` | fd (1 or 2) | buffer pointer | length | bytes written or `-EBADF` |
| 3 | `SYS_READ` | fd (0 = stdin; ≥3 = open file) | buffer pointer | max length | bytes read or error |
| 4 | `SYS_GETPID` | — | — | — | task ID |
| 5 | `SYS_YIELD` | — | — | — | 0 |
| 6 | `SYS_OPEN` | path pointer | flags | — | fd ≥ 3 or negative error |
| 7 | `SYS_CLOSE` | fd | — | — | 0 or negative error |
| 8 | `SYS_LSEEK` | fd | offset (signed) | whence | new offset or negative error |
| 9 | `SYS_STAT` | path pointer | `struct stat *` | — | 0 or negative error |
| 10 | `SYS_ACCESS` | path pointer | mode (`F_OK`/`R_OK`/`W_OK`/`X_OK`) | — | 0, `-EACCES`, or `-ENOENT` |
| 11 | `SYS_CHMOD` | path pointer | mode | — | `-EROFS` (infrastructure only, see below) |
| 12 | `SYS_CHOWN` | path pointer | uid | gid | `-EROFS` (infrastructure only, see below) |
| 13 | `SYS_READDIR` | path pointer | `struct dirent *` buffer | max entries | entry count or negative error |
| 14 | `SYS_DEBUG_SETCRED` | uid | gid | — | 0 (test hook — see below) |
| 15 | `SYS_READLINK` | path pointer | buffer pointer | buffer size | bytes copied or negative error |
| 16 | `SYS_LSTAT` | path pointer | `struct stat *` | — | 0 or negative error |
| 17 | `SYS_MKDIR` | path pointer | — | — | 0 or negative error |
| 18 | `SYS_UNLINK` | path pointer | — | — | 0 or negative error |
| 19 | `SYS_RMDIR` | path pointer | — | — | 0 or negative error |
| 20 | `SYS_RENAME` | old path pointer | new path pointer | — | 0 or negative error |
| 21 | `SYS_LINK` | old path pointer | new path pointer | — | 0 or negative error |
| 22 | `SYS_SYMLINK` | target pointer | path pointer | — | 0 or negative error |
| 23 | `SYS_FORK` | — | — | — | child's pid in the parent, `0` in the child, or `-1` |
| 24 | `SYS_EXEC` | path pointer | — | — | only returns (negative error) on failure |
| 25 | `SYS_WAIT` | pid (`-1` = any child) | `int *status` or NULL | — | reaped child's pid, or `-1` if no such child |

---

## SYS_EXIT (1)

Returns the value of `EBX` from `syscall_dispatch` back to `isr_dispatch`, which writes it to `regs->eax`. The `elf_exec` caller receives this as the exit code. **The current task is not terminated** — `task_exit()` is not called. The task resumes after `elf_exec` returns.

---

## SYS_WRITE (2)

Writes `len` bytes from `buf` to the console. Only file descriptors 1 (stdout) and 2 (stderr) are accepted; both route to `putchar` → `vga_putc`. Any other file descriptor returns `-EBADF`.

---

## SYS_READ (3)

Reads from `fd`.

**fd == 0 (stdin)**:

Reads up to `len` bytes from the keyboard into `buf`. Blocks on `keyboard_getkey()` until the user presses Enter. On Enter, a `\n` byte is appended and the read terminates. Extended key codes (values ≥ 128) and `KEY_NONE` (0) are discarded. No echo is performed.

**fd ≥ 3 (open file)**:

Reads up to `len` bytes from the current file offset of the open descriptor into `buf`. Advances the offset by the number of bytes copied. Returns 0 at EOF (offset ≥ file size). Zero-length reads are valid and return 0. The file content was loaded into a kernel buffer at `SYS_OPEN` time; no disk I/O occurs during `SYS_READ`.

**Error returns**:

| Code | Value | Condition |
|---|---|---|
| `-EINVAL` | -22 | null `buf` pointer |
| `-EBADF` | -9 | fd is 1 or 2 (write-only), or fd is outside [0, MAX_OPEN_FILES), or fd is ≥ 3 and not open |

---

## SYS_GETPID (4)

Returns `task_current()->id`. The initial kernel task has ID 1.

---

## SYS_YIELD (5)

Calls `task_yield()`, which finds the next ready task in the circular list and performs a context switch. Returns 0.

---

## SYS_OPEN (6)

Opens a file and allocates a file descriptor — read-only or writable, depending on flags.

**Arguments**:
- `EBX`: pointer to null-terminated absolute path string
- `ECX`: flags — `O_RDONLY` (0), or `O_WRONLY` (0x001) combined with any of `O_CREAT` (0x100) / `O_TRUNC` (0x200) / `O_APPEND` (0x400)

**Returns**: file descriptor (≥ 3) on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EINVAL` | -22 | null path pointer |
| `-ENOENT` | -2 | path does not exist, and (for a write open) `O_CREAT` wasn't given |
| `-EISDIR` | -21 | path refers to a directory |
| `-EACCES` | -13 | missing `R_OK` (read open) or `W_OK` (write open), or missing `X_OK` on an ancestor directory during resolution |
| `-ELOOP` | -40 | path resolution followed more than 40 symlinks (see `docs/api/vfs.md`'s Loop detection) |
| `-EMFILE` | -24 | all fd slots (3–15) are in use |
| *(create failure)* | | any negative errno `vfs_create()` returns, if `O_CREAT` was needed and failed for a reason other than the file already existing |

Both the initial `vfs_stat()`/`vfs_create()` check and the subsequent `vfs_read_file()` load return whatever negative errno pathname resolution actually produced — `SYS_OPEN` never collapses a resolution failure like `-ELOOP` or an ancestor's `-EACCES` down to a generic `-ENOENT`; only a genuinely missing path is reported as `-ENOENT`.

**Read opens** (`flags` without `O_WRONLY`, unchanged since Stage 3A): `vfs_read_file()` loads the entire file into a `kmalloc`'d buffer at open time; `SYS_READ` never touches disk.

**Write opens** (Stage 4, `flags & O_WRONLY`): if the path doesn't exist and `O_CREAT` is set, `vfs_create()` runs first (a harmless `-EEXIST` from a race is ignored — POSIX `creat()`/`open(O_CREAT)` without `O_EXCL` never errors just because the file already exists). The descriptor then starts with an **empty** in-memory buffer (`O_APPEND` instead pre-loads the existing content and seeks to its end) that `SYS_WRITE` grows as data is written; nothing reaches the filesystem until `SYS_CLOSE`. `pu_creat(path)` is `pu_open(path, O_WRONLY|O_CREAT|O_TRUNC)` — this kernel's equivalent of POSIX `creat()`.

**Descriptor allocation**: the lowest available index in the range 3–`MAX_OPEN_FILES-1` (16) is used. Indices 0, 1, 2 are permanently reserved for stdin, stdout, and stderr.

**Limitations**: path pointer is accepted without bounds-checking (no user/kernel memory separation); there is no `O_RDWR` (read-modify-write of an existing file requires read, then a separate truncating write).

---

## SYS_CLOSE (7)

Closes an open file descriptor, flushing buffered writes first, then frees its kernel buffer.

**Arguments**:
- `EBX`: file descriptor to close

**Returns**: `0` on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EBADF` | -9 | fd is not in range 3–15, or not currently open |
| *(flush failure)* | | any negative errno `vfs_write_file()` returns, if the descriptor was opened writable |

If the descriptor was opened with `O_WRONLY` (Stage 4), its entire in-memory write buffer is flushed to the filesystem via one `vfs_write_file()` call before the buffer is freed — the fd's whole write lifetime is "accumulate in memory, commit once on close," mirroring the read side's "load whole file into memory on open."

Descriptors 0, 1, and 2 cannot be closed. After a successful close the descriptor slot is zeroed and available for reuse.

---

## SYS_LSEEK (8)

Repositions the read offset of an open file descriptor.

**Arguments**:
- `EBX`: file descriptor
- `ECX`: offset (signed 32-bit integer)
- `EDX`: whence — one of `SEEK_SET` (0), `SEEK_CUR` (1), `SEEK_END` (2)

**Returns**: the new file offset on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EBADF` | -9 | fd is not in range 3–15, or not currently open |
| `-EINVAL` | -22 | unknown whence value, or resulting offset is negative |

**Seek semantics**:

| whence | New offset |
|---|---|
| `SEEK_SET` | `offset` |
| `SEEK_CUR` | `current_offset + offset` |
| `SEEK_END` | `file_size + offset` |

Seeking past the end of the file is allowed. Seeking before byte 0 returns `-EINVAL`.

---

## SYS_STAT (9)

Retrieves metadata for a file or directory by path.

**Arguments**:
- `EBX`: pointer to null-terminated absolute path string
- `ECX`: pointer to a `struct stat` buffer to fill

**Returns**: `0` on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EINVAL` | -22 | null path pointer or null stat pointer |
| `-ENOENT` | -2 | path does not exist on either filesystem |

**`struct stat` layout**:

```c
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = file, 2 = directory, 3 = symlink */
    unsigned short st_attr;   /* FAT attribute byte (legacy) */

    mode_t   st_mode;    /* file-type bits | rwx permission bits */
    uid_t    st_uid;
    gid_t    st_gid;
    unsigned int st_nlink;
    unsigned int st_ino;
    unsigned int st_atime;
    unsigned int st_mtime;
    unsigned int st_ctime;
    unsigned int st_blocks;   /* 512-byte blocks allocated */
    unsigned int st_blksize;  /* preferred I/O block size */
};
```

`st_mode`/`st_uid`/`st_gid`/`st_nlink`/`st_ino`/timestamps/`st_blocks`/`st_blksize` come directly from the EXT2 inode; FAT16 synthesizes them (uid=gid=0, mode derived from file/dir type, nlink=1 — see `docs/filesystem.md`). `st_atime`/`st_mtime`/`st_ctime` are 0 only if the underlying filesystem genuinely has no timestamp for that entry.

`vfs_stat` enforces `X_OK` on every ancestor directory in `path` before returning metadata (see `SYS_ACCESS` and `docs/api/vfs.md`'s Permissions section), and — Stage 4 — transparently follows a symlink named by `path`'s final component: `stat()` on a symlink describes what it points to, never the link itself. Use `SYS_LSTAT` for the link's own metadata. A path resolving through more than 40 symlink hops (including cycles like `A -> B -> A`) fails with `-ELOOP` instead of `-ENOENT`.

---

## SYS_ACCESS (10)

Checks whether the calling task's credentials would permit the requested access to `path`, mirroring POSIX `access(2)`.

**Arguments**:
- `EBX`: pointer to null-terminated absolute path string
- `ECX`: mode — `F_OK` (0), or an OR of `R_OK` (4) / `W_OK` (2) / `X_OK` (1)

**Returns**: `0` if access would be permitted, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EINVAL` | -22 | null path pointer |
| `-ENOENT` | -2 | path does not exist |
| `-EACCES` | -13 | path exists but the requested access is denied, or an ancestor directory denies `X_OK` during resolution |
| `-ELOOP` | -40 | path resolution followed more than 40 symlinks |

**Implementation**: `vfs_stat(path, &st)` followed by `vfs_access(&st, current_uid(), current_gid(), mode)` — the same central permission engine every other permission check in the kernel uses. `vfs_stat()`'s own return code (not just its 0/non-zero-ness) is passed straight back, so an ancestor's `-EACCES` or a symlink loop's `-ELOOP` reaches the caller as such rather than being reported as `-ENOENT`.

---

## SYS_CHMOD (11) / SYS_CHOWN (12)

Syscall infrastructure only — no filesystem stores mutable permission/ownership bits yet.

**SYS_CHMOD** — `EBX`: path pointer, `ECX`: mode. **SYS_CHOWN** — `EBX`: path pointer, `ECX`: uid, `EDX`: gid.

**Returns**: `-ENOENT` if the path doesn't exist, otherwise `-EROFS` (both EXT2 and FAT16 leave `ops->chmod`/`ops->chown` `NULL`). The syscalls exist now so a future writable filesystem can support them without any ABI change.

---

## SYS_READDIR (13)

Enumerates a directory's entries into a flat, caller-supplied buffer — no cursor/offset semantics; the whole directory (up to the buffer's capacity) is returned in one call.

**Arguments**:
- `EBX`: pointer to null-terminated absolute path string
- `ECX`: pointer to an array of `struct dirent`
- `EDX`: capacity of that array (max entries to write)

**Returns**: the number of entries written (`>= 0`), or a negative error code (`-EINVAL`, `-ENOENT`, `-EACCES`, `-ELOOP`) — the real code `vfs_readdir()`'s pathname resolution produced, not folded into a generic `-ENOENT`.

**`struct dirent` layout** (`include/pureunix/dirent.h` / `user/libpure.h`):

```c
struct dirent {
    char         name[64];   /* PUREUNIX_MAX_NAME */
    unsigned int type;       /* 1 = file, 2 = directory, 3 = symlink */
    unsigned int size;
};
```

`.` and `..` are included like any other entry — the VFS/driver layer never filters them; a shell's `ls -a` vs plain `ls` is purely a display-side choice made by whoever calls `SYS_READDIR`/`vfs_readdir`.

---

## SYS_DEBUG_SETCRED (14)

**Test-only.** Overwrites the calling task's `uid`/`gid` outright, with **no privilege check whatsoever** — there is no login system yet to check against, so this is not a real `setuid()`. It exists solely so `user/ext2test.c` can exercise owner/group/other permission logic while running as non-root. Nothing outside the regression suite should call it, and it must not be treated as a real syscall going forward.

**Arguments**: `EBX`: new uid, `ECX`: new gid. **Returns**: always `0`.

---

## SYS_READLINK (15)

Reads the raw target of a symlink, mirroring POSIX `readlink(2)`.

**Arguments**:
- `EBX`: pointer to the symlink's path
- `ECX`: buffer pointer
- `EDX`: buffer size

**Returns**: number of bytes copied (`>= 0`), never NUL-terminated and truncated to the buffer size if the target is longer; or a negative error code (`-EINVAL` if `path` isn't a symlink, `-ENOENT`, `-EACCES`, `-ELOOP`).

`path`'s own final component is never followed (it names the symlink, not its target) — only ancestor directories are resolved through any symlinks *they* contain.

---

## SYS_LSTAT (16)

Identical to `SYS_STAT` except the final path component is never followed — if it names a symlink, the returned `struct stat` describes the symlink itself (`st_type == 3`, `st_mode` has the `S_IFLNK` bit, permission bits read as the conventional `0777`).

**Arguments**: `EBX`: path pointer, `ECX`: `struct stat *`. **Returns**: `0` or a negative error code, same set as `SYS_STAT`.

---

## SYS_MKDIR (17) / SYS_UNLINK (18) / SYS_RMDIR (19)

Thin wrappers over `vfs_mkdir`/`vfs_unlink`/`vfs_rmdir` (`docs/api/vfs.md`) — each takes just a path pointer in `EBX` and returns `0` or the negative errno the VFS call produced.

| Syscall | Notable failure modes |
|---|---|
| `SYS_MKDIR` | `-EEXIST` (already exists), `-EACCES`, `-ENOSPC` |
| `SYS_UNLINK` | `-EISDIR` (use `SYS_RMDIR` instead), `-ENOENT`, `-EACCES` |
| `SYS_RMDIR` | `-ENOTDIR`, `-ENOTEMPTY`, `-ENOENT`, `-EACCES` |

`SYS_UNLINK`/`SYS_RMDIR` never follow a trailing symlink — they act on the named entry itself.

---

## SYS_RENAME (20)

**Arguments**: `EBX`: old path pointer, `ECX`: new path pointer. **Returns**: `0` or a negative errno (`vfs_rename`).

Neither path follows a trailing symlink. Both must resolve to the same mounted filesystem — `-EXDEV` otherwise (e.g. renaming from EXT2 `/` to FAT16 `/fat` always fails this way; there is no cross-filesystem move). If the destination already exists it is replaced (same-type only: file-for-file, empty-directory-for-empty-directory), with the moved inode's number unchanged. See `docs/filesystem.md`'s EXT2 write-path section for the on-disk entry-replacement and `..`-repointing details.

---

## SYS_LINK (21)

Creates a second directory entry (a hard link) referring to the same inode.

**Arguments**: `EBX`: existing path, `ECX`: new path. **Returns**: `0` or a negative errno.

The existing path is not followed if it's a symlink (the link points at the symlink inode itself, not its target). The new path must not already exist (`-EEXIST`), and must resolve to the same filesystem as the existing path (`-EXDEV`). Hard-linking a directory always fails with `-EPERM`.

---

## SYS_SYMLINK (22)

Creates a new symlink whose target text is stored verbatim (never validated or resolved at creation time — a symlink may point at nothing).

**Arguments**: `EBX`: target text pointer, `ECX`: new symlink path. **Returns**: `0` or a negative errno (`-EEXIST` if the path is already taken, `-EACCES`, `-ENOSPC`).

On EXT2, a target of 60 bytes or less is stored inline in the inode (a "fast symlink" — no data block allocated, `st_blocks == 0`); longer targets get a real data block chain like a regular file (`st_blocks > 0`). Both are transparent to every reader — `SYS_READLINK`/path resolution don't care which representation was used.

---

## SYS_FORK (23)

Duplicates the calling process, which must be a ring-3 task (kernel-mode callers get `-1`). See `docs/scheduler.md`'s "Forking" section for the full mechanism — in short: a new task with its own private, deep-copied address space (`docs/memory.md`) and deep-copied file descriptors, whose first scheduling resumes it in ring 3 exactly where the parent's `fork()` call was made.

**Returns**: the child's pid (`> 0`) in the parent, `0` in the child, or `-1` on failure (not a ring-3 caller, or out of memory).

---

## SYS_EXEC (24)

Replaces the calling process's own address space with the ELF at `path`, in place — the userspace-visible equivalent of POSIX `execve()`. Builds an entirely new page directory and loads the program into it before touching the caller's state, so a failure (bad path, bad ELF, permission denied, out of memory) leaves the caller completely unaffected and returns normally with a negative error code. On success, the caller's old address space is freed, CR3 is switched, and the very same `int $0x80` return path is redirected (by overwriting the trap frame's `eip`/`useresp`) to start the new program instead of resuming the old one — there is no observable "return" from a successful call.

**Arguments**: `EBX`: pointer to the ELF's path. **Returns**: only ever returns on failure (`-1`, or whatever negative errno the ELF loader produced — see `elf_load_into()` in `kernel/elf.c`).

Fork before exec (the classic `fork()` + `exec()` pattern) to run a new program while the caller keeps running — `elf_exec_current()` (this syscall's implementation) always replaces the *calling* task, never spawns a separate one.

---

## SYS_WAIT (25)

Blocks (cooperatively, via `task_yield()`) until a child of the caller becomes a zombie, then reaps it. See `docs/scheduler.md`'s "Waiting" section.

**Arguments**: `EBX`: pid to wait for (`-1` = any child of the caller), `ECX`: pointer to an `int` to receive the exit code, or NULL to discard it.

**Returns**: the reaped child's pid, or `-1` if the caller has no child matching `pid` (neither running nor already a zombie).

---

## Error Return

Any unrecognized syscall number returns `(uint32_t)-1`.

---

## Error Codes

Defined in `include/pureunix/errno.h` (kernel) and `user/libpure.h` (user programs):

| Constant | Value | Meaning |
|---|---|---|
| `EPERM` | 1 | operation not permitted (e.g. `SYS_LINK` on a directory) |
| `ENOENT` | 2 | no such file or directory |
| `EIO` | 5 | I/O error |
| `EBADF` | 9 | bad file descriptor |
| `EACCES` | 13 | permission denied |
| `EEXIST` | 17 | file already exists |
| `EXDEV` | 18 | cross-device link/rename (e.g. EXT2 -> FAT16) |
| `ENOTDIR` | 20 | not a directory |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `EMFILE` | 24 | too many open files |
| `ENOSPC` | 28 | no space left on device (allocator exhausted) |
| `EROFS` | 30 | read-only filesystem |
| `ENAMETOOLONG` | 36 | path or component too long |
| `ENOSYS` | 38 | function not implemented |
| `ENOTEMPTY` | 39 | directory not empty (`SYS_RMDIR`) |
| `ELOOP` | 40 | too many symbolic links encountered during resolution (> 40 follows) |

Kernel returns `(uint32_t)-CODE`; user receives a negative `int`.

---

## Unimplemented Syscalls

`fstat`, `mmap`, `munmap`, `brk`, `kill`, `signal`, `pipe`, `dup`, `dup2`, `chdir`, `getcwd`, `setuid`, `setgid`, `getuid`, `getgid`.

(`mkdir`, `unlink`, `rmdir`, `rename`, `link`, `symlink`, and `readlink` were added in Stage 4 — see `SYS_MKDIR`, `SYS_UNLINK`, `SYS_RMDIR`, `SYS_RENAME`, `SYS_LINK`, `SYS_SYMLINK`, and `SYS_READLINK` above. `fork`, `exec`, and `wait`/`waitpid` were added alongside per-process address spaces — see `SYS_FORK`, `SYS_EXEC`, and `SYS_WAIT` above.)

`SYS_CHMOD`/`SYS_CHOWN` exist as syscall numbers but currently always return `-EROFS` — see above.

---

## libpure Wrappers

`user/libpure.c`:

```c
int    pu_write(int fd, const char *buf, size_t len);           // SYS_WRITE
int    pu_read(int fd, char *buf, size_t len);                  // SYS_READ
int    pu_open(const char *path, int flags);                    // SYS_OPEN
int    pu_close(int fd);                                        // SYS_CLOSE
int    pu_lseek(int fd, int offset, int whence);                // SYS_LSEEK
int    pu_stat(const char *path, struct stat *st);              // SYS_STAT
int    pu_access(const char *path, int mode);                   // SYS_ACCESS
int    pu_chmod(const char *path, mode_t mode);                 // SYS_CHMOD
int    pu_chown(const char *path, uid_t uid, gid_t gid);         // SYS_CHOWN
int    pu_readdir(const char *path, struct dirent *entries,
                   int max_entries);                            // SYS_READDIR
int    pu_debug_setcred(uid_t uid, gid_t gid);                  // SYS_DEBUG_SETCRED — test-only, see above
int    pu_fork(void);                                           // SYS_FORK
int    pu_exec(const char *path);                               // SYS_EXEC
int    pu_wait(int pid, int *status);                           // SYS_WAIT
void   pu_exit(int code);           // int $0x81 directly — see docs/scheduler.md's SYS_EXIT note
void   pu_puts(const char *s);                                  // SYS_WRITE to fd 1
void   pu_puti(int value);                                      // integer to decimal on fd 1
size_t pu_strlen(const char *s);                                // no syscall
int    pu_atoi(const char *s);                                  // no syscall
```

`pu_puts` calls `pu_write(1, s, pu_strlen(s))`.
All syscall wrappers forward directly to `syscall3` — no inline assembly beyond the shared `syscall3` helper.
