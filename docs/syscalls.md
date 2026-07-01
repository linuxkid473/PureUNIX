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

Opens a file for reading and allocates a file descriptor.

**Arguments**:
- `EBX`: pointer to null-terminated absolute path string
- `ECX`: flags — only `O_RDONLY` (0) is accepted

**Returns**: file descriptor (≥ 3) on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EINVAL` | -22 | null path pointer, or flags other than `O_RDONLY` |
| `-ENOENT` | -2 | path does not exist on either filesystem |
| `-EISDIR` | -21 | path refers to a directory |
| `-EMFILE` | -24 | all fd slots (3–15) are in use |

**Descriptor allocation**: the lowest available index in the range 3–`MAX_OPEN_FILES-1` (16) is used. Indices 0, 1, 2 are permanently reserved for stdin, stdout, and stderr.

**Implementation**: `vfs_read_file()` is called at open time (EXT2-first, then FAT16 fallback), loading the entire file into a `kmalloc`'d buffer. The buffer and a read offset are stored in the task's `fd_entry_t`.

**Limitations**:
- `O_RDONLY` only; `O_WRONLY` and `O_RDWR` return `-EINVAL`.
- Path pointer is accepted without bounds-checking (no user/kernel memory separation).

---

## SYS_CLOSE (7)

Closes an open file descriptor and frees its kernel buffer.

**Arguments**:
- `EBX`: file descriptor to close

**Returns**: `0` on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EBADF` | -9 | fd is not in range 3–15, or not currently open |

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

`vfs_stat` also enforces `X_OK` on every ancestor directory in `path` before returning metadata — see `SYS_ACCESS` and `docs/api/vfs.md`'s Permissions section for the full model.

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
| `-ENOENT` | -2 | path does not exist (or an ancestor directory denies `X_OK`, folded into the same `vfs_stat` failure) |
| `-EACCES` | -13 | path exists but the requested access is denied |

**Implementation**: `vfs_stat(path, &st)` followed by `vfs_access(&st, current_uid(), current_gid(), mode)` — the same central permission engine every other permission check in the kernel uses.

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

**Returns**: the number of entries written (`>= 0`), or a negative error code (`-EINVAL`, `-ENOENT`).

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

## Error Return

Any unrecognized syscall number returns `(uint32_t)-1`.

---

## Error Codes

Defined in `include/pureunix/errno.h` (kernel) and `user/libpure.h` (user programs):

| Constant | Value | Meaning |
|---|---|---|
| `ENOENT` | 2 | no such file or directory |
| `EIO` | 5 | I/O error |
| `EBADF` | 9 | bad file descriptor |
| `EACCES` | 13 | permission denied |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `EMFILE` | 24 | too many open files |
| `EROFS` | 30 | read-only filesystem |
| `ENOSYS` | 38 | function not implemented |

Kernel returns `(uint32_t)-CODE`; user receives a negative `int`.

---

## Unimplemented Syscalls

`fstat`, `mmap`, `munmap`, `brk`, `fork`, `exec`, `wait`, `waitpid`, `kill`, `signal`, `pipe`, `dup`, `dup2`, `chdir`, `getcwd`, `mkdir`, `unlink`, `rename`, `readlink`, `symlink`, `setuid`, `setgid`, `getuid`, `getgid`.

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
void   pu_puts(const char *s);                                  // SYS_WRITE to fd 1
void   pu_puti(int value);                                      // integer to decimal on fd 1
size_t pu_strlen(const char *s);                                // no syscall
int    pu_atoi(const char *s);                                  // no syscall
```

`pu_puts` calls `pu_write(1, s, pu_strlen(s))`.
All syscall wrappers forward directly to `syscall3` — no inline assembly beyond the shared `syscall3` helper.
