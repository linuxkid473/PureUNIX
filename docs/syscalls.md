# System Calls

## Overview

User-space programs invoke the kernel via `int $0x80`. The syscall number is passed in `EAX`; up to three arguments in `EBX`, `ECX`, `EDX`. The return value is written back to `EAX`.

The dispatch function is `syscall_dispatch` in `arch/i386/syscall.c`. It is called by `isr_dispatch` when vector 0x80 fires.

---

## Path Resolution

Every syscall that takes a path (`SYS_OPEN`, `SYS_STAT`, `SYS_ACCESS`, `SYS_LSTAT`, `SYS_READLINK`, `SYS_MKDIR`, `SYS_UNLINK`, `SYS_RMDIR`, `SYS_RENAME`, `SYS_LINK`, `SYS_SYMLINK`'s second argument, `SYS_CHMOD`, `SYS_CHOWN`, `SYS_UTIME`, `SYS_READDIR`, and the path `SYS_EXEC`/`elf_exec_argv()`/`elf_exec_current()` load) resolves a relative path against the *calling task's own* current working directory (`task_current_cwd()`, `include/pureunix/task.h`) via `vfs_normalize()` (`fs/vfs.c`) before doing anything else — `arch/i386/syscall.c`'s `resolve_path()` helper. An absolute path (starting with `/`) is unaffected (`vfs_normalize()` just canonicalizes `.`/`..` components in it).

This wasn't always true: originally every one of these syscalls handed its raw path straight to `vfs_*()`, which happened to work only because every caller either always used absolute paths (every regression test) or pre-resolved relative ones itself before calling anything (the in-kernel shell's builtins, `shell/sh.c`, via their own `ctx->cwd` and `vfs_normalize()` call). Real, independent multi-process programs — BusyBox's coreutils, ash — make raw relative-path syscalls directly (a bare `stat(".")`, `open("foo")`), which surfaced the gap as a real, user-visible bug (`ls` with no arguments failing "No such file or directory", since `.` was being treated as relative to the filesystem root rather than the caller's cwd). Fixed by generalizing `SYS_CHDIR`'s own (always-correct) resolution pattern to every other path-taking syscall.

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
| 11 | `SYS_CHMOD` | path pointer | mode | — | 0, or negative error (real on EXT2, `-EROFS` on FAT16 — see below) |
| 12 | `SYS_CHOWN` | path pointer | uid | gid | 0, or negative error (real on EXT2, `-EROFS` on FAT16 — see below) |
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
| 24 | `SYS_EXEC` | path pointer | `argv[]` pointer, or 0 | `envp[]` pointer, or 0 | only returns (negative error) on failure |
| 25 | `SYS_WAIT` | pid (`-1` = any child) | `int *status` or NULL | — | reaped child's pid, or `-1` if no such child |
| 26 | `SYS_TCGETATTR` | fd (0, 1, or 2) | `struct termios *` | — | 0 or negative error |
| 27 | `SYS_TCSETATTR` | fd (0, 1, or 2) | `struct termios *` | actions (`TCSANOW`/`TCSADRAIN`/`TCSAFLUSH`) | 0 or negative error |
| 28 | `SYS_IOCTL` | fd (0, 1, or 2) | request (`TIOCGWINSZ`) | `struct winsize *` | 0 or negative error |
| 29 | `SYS_CHDIR` | path pointer | — | — | 0 or negative error |
| 30 | `SYS_GETCWD` | buffer pointer | buffer size | — | 0 or negative error |
| 31 | `SYS_NANOSLEEP` | pointer to requested `struct pureunix_timespec` | pointer to remaining-time output, or NULL | — | 0 or negative error |
| 32 | `SYS_GETUID` | — | — | — | the caller's uid |
| 33 | `SYS_GETGID` | — | — | — | the caller's gid |
| 34 | `SYS_UTIME` | path pointer | atime (or `0xFFFFFFFF`) | mtime (or `0xFFFFFFFF`) | 0 or negative error |
| 35 | `SYS_GETTIMEOFDAY` | pointer to `uint32_t` | — | — | 0 or negative error |
| 36 | `SYS_PIPE` | pointer to `int[2]` output | — | — | 0 or negative error |
| 37 | `SYS_DUP` | fd to duplicate | — | — | new fd or negative error |
| 38 | `SYS_DUP2` | fd to duplicate | fd to become a copy of it | — | newfd or negative error |
| 39 | `SYS_KILL` | pid | signal number (0 = null signal) | — | 0, or negative error (never returns for `pid == self` with a nonzero signal) |
| 40 | `SYS_FCNTL` | fd | cmd (`F_DUPFD`/`F_DUPFD_CLOEXEC`/`F_GETFD`/`F_SETFD`/`F_GETFL`/`F_SETFL`) | arg | cmd-dependent or negative error |
| 41 | `SYS_FTRUNCATE` | fd | new length | — | 0 or negative error |
| 42 | `SYS_GETPPID` | — | — | — | parent task ID (0 if none) |
| 43 | `SYS_PING` | destination IPv4 (host order) | timeout (ms) | `uint32_t *` RTT out, or 0 | 0 on reply, `-ETIMEDOUT` otherwise |
| 44 | `SYS_FSTAT` | fd | `struct stat *` | — | 0 or negative error |

---

## SYS_EXIT (1)

Returns the value of `EBX` from `syscall_dispatch` back to `isr_dispatch`, which writes it to `regs->eax`. The `elf_exec` caller receives this as the exit code. **The current task is not terminated** — `task_exit()` is not called. The task resumes after `elf_exec` returns.

---

## SYS_WRITE (2)

Writes `len` bytes from `buf`. fds 1 and 2 route to `putchar` → `vga_putc` (the console) *as long as they still hold their default console binding* — see "File descriptors are now shared, refcounted open file descriptions" below; `SYS_DUP2` can redirect either of them to a real open file or pipe, exactly like a real UNIX process's stdout/stderr redirection, in which case they behave like any other writable fd instead. Any fd outside `[0, MAX_OPEN_FILES)`, or one that isn't currently open, returns `-EBADF`.

---

## SYS_READ (3)

Reads from `fd`.

**fd == 0 (stdin), still holding its default console binding** (see "File descriptors are now shared, refcounted open file descriptions" below — `SYS_DUP2` can redirect fd 0 to a real file or pipe, in which case it's read like any other fd instead):

Routed through `tty_read()` (`drivers/tty.c`), which applies whatever `struct termios` is currently in effect for the console — see `SYS_TCGETATTR`/`SYS_TCSETATTR` below. By default (canonical mode, `ICANON|ECHO|ECHOE|ISIG`):

Reads up to `len` bytes from the keyboard into `buf`, echoing each character as it's typed and honoring `VERASE`/`VKILL`/`VEOF` line editing. Blocks until the user presses Enter (a trailing `\n` is included in the returned bytes) or `VEOF` (Ctrl-D) is pressed on an empty line (returns 0). `VINTR` (Ctrl-C) aborts the read and returns `-EINTR`. Extended key codes with no ASCII meaning (arrows, F-keys, ...) are silently dropped.

In raw mode (`ICANON` clear), returns as soon as at least one byte is available (blocking for the first byte, then draining whatever else is already queued without blocking further), without line editing; echo still follows `ECHO`. See `docs/api/drivers.md` for `tty_read()`'s exact contract.

Note for anyone adding future syscalls that can block waiting on the keyboard: `int $0x80` runs with interrupts masked (see `arch/i386/interrupt_stubs.S`'s `isr128`), so a blocking read must call `arch_enable_interrupts()` before waiting — otherwise `keyboard_getkey()`'s `hlt` loop can never be woken by the keyboard IRQ. `tty_read()` does this already.

**fd ≥ 3, or fd 0/1/2 redirected via `SYS_DUP2` (a real open file or pipe)**:

For `FD_KIND_FILE`: reads up to `len` bytes from the current file offset into `buf`. Advances the offset by the number of bytes copied — shared with every other fd referencing the same open file description (see below), not just this one. Returns 0 at EOF (offset ≥ file size). Zero-length reads are valid and return 0. The file content was loaded into a kernel buffer at `SYS_OPEN` time; no disk I/O occurs during `SYS_READ`.

For `FD_KIND_PIPE` (the read end of a `SYS_PIPE`): see `SYS_PIPE` below.

**Error returns**:

| Code | Value | Condition |
|---|---|---|
| `-EINVAL` | -22 | null `buf` pointer |
| `-EBADF` | -9 | fd 1/2 still console-bound (write-only), fd outside `[0, MAX_OPEN_FILES)`, fd not open, or (pipe) reading from the write end |

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
- `EBX`: pointer to null-terminated path string (relative paths are resolved against the caller's cwd — see "Path Resolution" above)
- `ECX`: flags — `O_RDONLY` (0), or `O_WRONLY` (0x001) combined with any of `O_CREAT` (0x100) / `O_TRUNC` (0x200) / `O_APPEND` (0x400)

**Returns**: file descriptor on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EINVAL` | -22 | null path pointer |
| `-ENOENT` | -2 | path does not exist, and (for a write open) `O_CREAT` wasn't given |
| `-EISDIR` | -21 | path refers to a directory |
| `-EACCES` | -13 | missing `R_OK` (read open) or `W_OK` (write open), or missing `X_OK` on an ancestor directory during resolution |
| `-ELOOP` | -40 | path resolution followed more than 40 symlinks (see `docs/api/vfs.md`'s Loop detection) |
| `-EMFILE` | -24 | no fd slot (0–15) is free — see "Descriptor allocation" below |
| *(create failure)* | | any negative errno `vfs_create()` returns, if `O_CREAT` was needed and failed for a reason other than the file already existing |

Both the initial `vfs_stat()`/`vfs_create()` check and the subsequent `vfs_read_file()` load return whatever negative errno pathname resolution actually produced — `SYS_OPEN` never collapses a resolution failure like `-ELOOP` or an ancestor's `-EACCES` down to a generic `-ENOENT`; only a genuinely missing path is reported as `-ENOENT`.

**Read opens** (`flags` without `O_WRONLY`, unchanged since Stage 3A): `vfs_read_file()` loads the entire file into a `kmalloc`'d buffer at open time; `SYS_READ` never touches disk.

**Write opens** (Stage 4, `flags & O_WRONLY`): if the path doesn't exist and `O_CREAT` is set, `vfs_create()` runs first (a harmless `-EEXIST` from a race is ignored — POSIX `creat()`/`open(O_CREAT)` without `O_EXCL` never errors just because the file already exists). The descriptor then starts with an **empty** in-memory buffer (`O_APPEND` instead pre-loads the existing content and seeks to its end) that `SYS_WRITE` grows as data is written; nothing reaches the filesystem until `SYS_CLOSE`. `pu_creat(path)` is `pu_open(path, O_WRONLY|O_CREAT|O_TRUNC)` — this kernel's equivalent of POSIX `creat()`.

**Descriptor allocation**: `arch/i386/syscall.c`'s `find_free_fd()` — the lowest available index in `[0, MAX_OPEN_FILES)` (16), real POSIX "lowest available fd" semantics. Indices 3–15 are available whenever unused. Indices 0/1/2 (stdin/stdout/stderr) are available **only if they were explicitly `SYS_CLOSE`'d** (`fd_entry_t.closed_explicitly`, `include/pureunix/task.h`) — never touched, or console-bound again after a `SYS_DUP2`-based redirect-then-restore cycle, are both *not* eligible, even though both also read as `used == true, file == NULL`. This distinction (added alongside the BusyBox port, see `docs/userland.md`'s "Platform Work This Needed") is what makes the standard `close(0); open(path)` idiom (BusyBox's `uniq FILE` and other coreutils' optional-`FILE`-argument handling) actually hand back fd 0, while *not* letting an unrelated `open()` steal a shell's own stdout out from under a redirect it's mid-restore on.

**Limitations**: path pointer is accepted without bounds-checking (no user/kernel memory separation); there is no `O_RDWR` (read-modify-write of an existing file requires read, then a separate truncating write).

---

## SYS_CLOSE (7)

Closes an open file descriptor, flushing buffered writes first, then frees its kernel buffer.

**Arguments**:
- `EBX`: file descriptor to close

**Returns**: `0` on success, or a negative error code:

| Code | Value | Meaning |
|---|---|---|
| `-EBADF` | -9 | fd is outside `[0, MAX_OPEN_FILES)`, or not currently open |
| *(flush failure)* | | any negative errno `vfs_write_file()` returns, if this was the *last* reference (see below) to a descriptor opened writable |

This drops one reference to the fd's underlying open file description (`open_file_unref()`, `kernel/task.c`) — see "File descriptors are now shared, refcounted open file descriptions" below. Only once the *last* reference is gone does an `O_WRONLY` `FD_KIND_FILE`'s entire in-memory write buffer actually get flushed to the filesystem via one `vfs_write_file()` call (the fd's whole write lifetime is "accumulate in memory, commit once on the last close," mirroring the read side's "load whole file into memory on open"); a `FD_KIND_PIPE` end similarly only retires once every fd referencing that same end (across every task that reached it via `dup()`/`dup2()`/`fork()`) has been closed.

Descriptors 0, 1, and 2 *can* now be closed (unlike before `SYS_DUP2` existed) — doing so reverts that slot to the default console binding (`file = NULL`) rather than leaving it genuinely unusable, and sets `fd_entry_t.closed_explicitly = true`, which is what makes it (and only it, not the console binding a never-touched or dup2()-restored 0/1/2 slot also has) eligible for the next `SYS_OPEN`/`SYS_PIPE`/`SYS_DUP`/`SYS_FCNTL`(`F_DUPFD`) allocation to reclaim — see `SYS_OPEN`'s "Descriptor allocation" above. Fd slots ≥ 3 become fully free the same way they always did (`used = false`).

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
- `EBX`: pointer to null-terminated path string (relative paths are resolved against the caller's cwd — see "Path Resolution" above)
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
- `EBX`: pointer to null-terminated path string (relative paths are resolved against the caller's cwd — see "Path Resolution" above)
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

Real on EXT2 (`fs/ext2/mount.c`'s `ext2_chmod()`/`ext2_chown()` mutate the inode's `i_mode`/`i_uid`/`i_gid` directly and persist it), still `-EROFS` on FAT16 (no Unix ownership/permission concept exists there at all, so `ops->chmod`/`ops->chown` stay `NULL`).

**SYS_CHMOD** — `EBX`: path pointer, `ECX`: mode (only the low 12 bits — permission + setuid/setgid/sticky — are used; the on-disk file-type bits are preserved untouched no matter what's passed). **SYS_CHOWN** — `EBX`: path pointer, `ECX`: uid, `EDX`: gid; either can be `(uid_t)-1`/`(gid_t)-1` to mean "leave unchanged" (POSIX `chown(2)`'s convention, e.g. changing only the group).

**Returns**: `0` on success, or a negative error code:

| Code | Condition |
|---|---|
| `-ENOENT` | the path doesn't exist |
| `-EROFS` | the target filesystem doesn't support chmod/chown at all (FAT16) |
| `-EPERM` | `SYS_CHMOD`: the caller is neither the file's owner nor root. `SYS_CHOWN`: the caller isn't root — chown is root-only outright, since PureUNIX's single-uid/gid-per-task model has no supplementary-group concept that would let a non-root owner hand a file to a group they belong to |

Permission checks happen in `fs/vfs.c`'s `vfs_chmod()`/`vfs_chown()`, before the filesystem-specific `ops->chmod`/`ops->chown` is ever called — same `current_uid()`/`current_gid()` pattern every other permission-checked VFS entry point uses.

---

## SYS_READDIR (13)

Enumerates a directory's entries into a flat, caller-supplied buffer — no cursor/offset semantics; the whole directory (up to the buffer's capacity) is returned in one call.

**Arguments**:
- `EBX`: pointer to null-terminated path string (relative paths are resolved against the caller's cwd — see "Path Resolution" above)
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

**Arguments**: `EBX`: pointer to the ELF's path. `ECX`: pointer to a NULL-terminated `argv[]` array (in the caller's own memory), or `0` for the bare `{path, NULL}` argv `pu_exec()` has always passed. `EDX`: pointer to a NULL-terminated `envp[]` array of `"KEY=VALUE"` strings (in the caller's own memory), or `0` for an empty environment. **Returns**: only ever returns on failure (`-1`, or whatever negative errno the ELF loader produced — see `elf_load_into()` in `kernel/elf.c`).

`argv`/`envp` are read directly out of the caller's own address space — safe because the kernel only switches CR3 to the new program's page directory *after* every string and pointer has already been copied onto the new stack (see `elf_load_into()`/`build_argv_stack()` in `kernel/elf.c`), so there's no window where the old pointers are read through the wrong mapping. `argc` isn't a separate argument; the kernel counts `argv[]` up to `ELF_MAX_ARGS` (16) entries by scanning for the NULL terminator, same as any real `execve()`. A NULL `envp` means a genuinely empty environment, not "inherit the caller's" — there is no per-process environment state to inherit from (a forked child only sees env vars its parent explicitly passed to `SYS_EXEC`).

Fork before exec (the classic `fork()` + `exec()` pattern) to run a new program while the caller keeps running — `elf_exec_current()` (this syscall's implementation) always replaces the *calling* task, never spawns a separate one.

---

## SYS_WAIT (25)

Blocks (cooperatively, via `task_yield()`) until a child of the caller becomes a zombie, then reaps it. See `docs/scheduler.md`'s "Waiting" section.

**Arguments**: `EBX`: pid to wait for (`-1` = any child of the caller), `ECX`: pointer to an `int` to receive the exit code, or NULL to discard it.

**Returns**: the reaped child's pid, or `-1` if the caller has no child matching `pid` (neither running nor already a zombie).

The `int` written to `*status` is the child's bare exit code (e.g. a child that called `pu_exit(7)` leaves `7` there) — not Linux's `(code << 8)`-style encoding, since there's no signal-terminated case to distinguish yet (see this doc's "Unimplemented Syscalls" section's note on `kill`/`signal`). `pu_wait()` and `user/systest.c`'s regression coverage both expect this raw form. Newlib's `wait()`/`waitpid()` (`user/newlib_syscalls.c`) translate it to the Linux-style encoding at the libc boundary — `(code & 0xff) << 8` — purely so newlib's own `<sys/wait.h>` `WIFEXITED`/`WEXITSTATUS` macros (which assume that encoding) work correctly; the kernel ABI itself is unaffected.

---

## SYS_TCGETATTR (26) / SYS_TCSETATTR (27)

Get/set the console's `struct termios` (`include/pureunix/termios.h`). PureUNIX has exactly one terminal — the VGA console fed by the PS/2 keyboard — so this is process-independent, kernel-global state (`drivers/tty.c`'s `console_termios`) shared by fds 0, 1, and 2, not per-open-file-description state like a real UNIX's tty layer.

**Arguments** (both): `EBX`: fd, must be 0, 1, or 2. `ECX`: `struct termios *`. `SYS_TCSETATTR` additionally takes `EDX`: `actions` (`TCSANOW`/`TCSADRAIN`/`TCSAFLUSH`) — all three apply immediately, since there is no pending-output queue or unread-input buffer to drain or flush; the argument only exists so the syscall's shape matches POSIX.

**Returns**: 0 on success, or a negative error.

**Error returns**:

| Code | Value | Condition |
|---|---|---|
| `-EINVAL` | -22 | null `struct termios *`, or (SYS_TCSETATTR only) `actions` is not `TCSANOW`/`TCSADRAIN`/`TCSAFLUSH` |
| `-EBADF` | -9 | fd is outside [0, `MAX_OPEN_FILES`), or fd is ≥ 3 and not an open descriptor |
| `-ENOTTY` | -25 | fd is ≥ 3 and *is* an open descriptor, just not a terminal (it's a regular file) |

Changing `ICANON`/`ECHO` takes effect on the very next `SYS_READ` — see `SYS_READ`'s fd == 0 case above.

---

## SYS_IOCTL (28)

Generic device control. PureUNIX implements exactly one request: `TIOCGWINSZ`, which reports the console's fixed 80x25 VGA text-grid size (`vga_get_size()`, `drivers/vga.c`) — there is no resize event, so this never changes at runtime.

**Arguments**: `EBX`: fd, must be 0, 1, or 2. `ECX`: request (`TIOCGWINSZ`, the only supported value). `EDX`: pointer to a `struct winsize` to fill.

```c
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel; /* unused: no pixel-accurate console geometry */
    unsigned short ws_ypixel; /* unused, same reason */
};
```

**Returns**: `0` on success, or a negative error code:

| Code | Value | Condition |
|---|---|---|
| `-EINVAL` | -22 | `request` is not `TIOCGWINSZ`, or the `struct winsize *` is null |
| `-EBADF` | -9 | fd is outside [0, `MAX_OPEN_FILES`), or fd is ≥ 3 and not an open descriptor |
| `-ENOTTY` | -25 | fd is ≥ 3 and *is* an open descriptor, just not a terminal (it's a regular file) |

There is no dedicated `isatty()` syscall — `pu_isatty(fd)` (`user/libpure.c`) is implemented as `pu_tcgetattr(fd, &t) == 0`, mirroring how real UNIX `isatty()` is conventionally just a `tcgetattr`/`ioctl(TCGETS)` call that succeeds or fails.

---

## SYS_CHDIR (29) / SYS_GETCWD (30)

Per-task working directory, stored as `task_t.cwd` (`include/pureunix/task.h`) — a child inherits its creator's cwd at `task_create()`/`task_fork()` time, exactly like `uid`/`gid`, and a `SYS_EXEC` in place (replacing the calling task's own image) leaves it untouched, since it's a process property, not something the ELF image carries. There is exactly one task acting as "the shell" (the kernel never spawns a separate task for it — see `docs/scheduler.md`), so `shell/sh.c`'s own `cd` builtin calls `task_set_cwd()` (`kernel/task.c`) after updating its own `shell_context_t.cwd`, keeping the two in sync; that's what makes a program launched from the shell start in the directory the shell was actually in, rather than always `/`.

**SYS_CHDIR** — **Arguments**: `EBX`: pointer to the target path (resolved relative to the caller's current cwd via `vfs_normalize()`, same as every other path-taking syscall). **Returns**: `0` on success, or a negative error code:

| Code | Condition |
|---|---|
| `-EINVAL` | null path pointer |
| `-ENOENT` | no filesystem mounted yet, or the resolved path doesn't exist |
| `-ENOTDIR` | the resolved path exists but isn't a directory |
| `-EACCES` | the resolved directory lacks `X_OK` (search permission) for the caller |
| `-ENAMETOOLONG` | the resolved absolute path doesn't fit in the task's cwd buffer (`PUREUNIX_MAX_PATH`, 256 bytes) |

**SYS_GETCWD** — **Arguments**: `EBX`: buffer pointer. `ECX`: buffer size in bytes. **Returns**: `0` on success (the NUL-terminated absolute cwd is copied into the buffer), or a negative error code:

| Code | Condition |
|---|---|
| `-EINVAL` | null buffer pointer, or size `0` |
| `-ERANGE` | the cwd (plus its NUL terminator) doesn't fit in the caller's buffer |

Unlike POSIX `getcwd(3)`, there is no `buf == NULL` auto-allocating mode — the caller must always supply a real buffer.

---

## SYS_NANOSLEEP (31)

Blocks the calling task for a requested duration, backed directly by the PIT tick counter's busy-halt loop (`arch/i386/pit.c`'s `pit_sleep()`, already used internally since before userspace could reach it) — the same cooperative-scheduling caveat as everything else non-preemptive applies: nothing else runs while this task sleeps.

**Arguments**: `EBX`: pointer to a `struct pureunix_timespec { long tv_sec; long tv_nsec; }` (`include/pureunix/time.h`) requesting the sleep duration. `ECX`: pointer to a `struct pureunix_timespec` to receive the remaining time if the sleep is interrupted early, or `NULL` to discard it — always written as `{0, 0}` here, since PureUNIX has no signal delivery yet to interrupt a sleep early (see "Unimplemented Syscalls" below); a `SYS_NANOSLEEP` call always runs to completion.

**Returns**: `0` on success, or `-EINVAL` if the request pointer is null or its fields are out of range (`tv_sec < 0`, or `tv_nsec` not in `[0, 1000000000)`).

`user/newlib_syscalls.c`'s `nanosleep()` passes `struct timespec` straight through with no translation (layout-compatible: both `{long tv_sec; long tv_nsec;}` on i686). `sleep()` is built on top of it.

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
| `EINTR` | 4 | a blocking read was aborted by `VINTR` (Ctrl-C) with `ISIG` set |
| `EIO` | 5 | I/O error |
| `EBADF` | 9 | bad file descriptor |
| `EACCES` | 13 | permission denied |
| `EEXIST` | 17 | file already exists |
| `EXDEV` | 18 | cross-device link/rename (e.g. EXT2 -> FAT16) |
| `ENOTDIR` | 20 | not a directory |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `EMFILE` | 24 | too many open files |
| `ENOTTY` | 25 | `SYS_TCGETATTR`/`SYS_TCSETATTR` on a valid, open, but non-terminal fd |
| `ENOSPC` | 28 | no space left on device (allocator exhausted) |
| `EROFS` | 30 | read-only filesystem |
| `ENAMETOOLONG` | 36 | path or component too long |
| `ENOSYS` | 38 | function not implemented |
| `ENOTEMPTY` | 39 | directory not empty (`SYS_RMDIR`) |
| `ELOOP` | 40 | too many symbolic links encountered during resolution (> 40 follows) |

Kernel returns `(uint32_t)-CODE`; user receives a negative `int`.

---

## SYS_GETUID (32) / SYS_GETGID (33)

Read-only credential getters — no arguments, the task's own `uid`/`gid` (`include/pureunix/task.h`) returned directly as the syscall's return value. The write side is still `SYS_DEBUG_SETCRED` (test-only, no privilege check — see above) until a real login/setuid model exists.

There is no separate "effective" uid/gid syscall: PureUNIX has no setuid model, so a real or effective ID never diverges — `geteuid()`/`getegid()` (`user/newlib_syscalls.c`) are simply aliases for `getuid()`/`getgid()`.

---

## SYS_UTIME (34)

Sets a file's atime/mtime directly. Real on EXT2 (`fs/ext2/mount.c`'s `ext2_utime()` mutates the inode's `i_atime`/`i_mtime` and persists it); `-EROFS` on FAT16 (no mutable timestamp storage there at all), same story as `SYS_CHMOD`/`SYS_CHOWN`.

**Arguments**: `EBX`: path pointer. `ECX`: atime (Unix epoch seconds), or `0xFFFFFFFF` to leave it unchanged. `EDX`: mtime, same convention. **Returns**: `0` on success, or a negative error code (`-EINVAL` null path, `-ENOENT` missing path, `-EROFS` unsupported filesystem, `-EPERM` — caller is neither the file's owner, root, nor holds `W_OK` on it, the traditional Unix `utime(2)` rule, checked in `fs/vfs.c`'s `vfs_utime()` before the filesystem-specific call).

`user/newlib_syscalls.c`'s `utimensat()`/`utimes()` both translate down to this — `utimensat()` maps `UTIME_NOW`/`UTIME_OMIT` sentinels to "current time"/"leave unchanged", and ignores `dirfd` (every PureUNIX path syscall already resolves relative to the caller's own cwd — see `SYS_CHDIR` — so there's no other fd-relative base to honor) and `AT_SYMLINK_NOFOLLOW` (no "don't follow the final symlink" variant exists, same simplification `lchown()` makes for `SYS_CHOWN`).

---

## SYS_GETTIMEOFDAY (35)

Wall-clock time as a Unix epoch second count — a thin userspace-visible wrapper around `kernel/time.c`'s `time_now()`, the same clock every write-path timestamp (EXT2 `i_atime`/`i_mtime`/`i_ctime`, `SYS_UTIME` above) already uses internally. No sub-second resolution exists.

**Arguments**: `EBX`: pointer to a `uint32_t` to receive the epoch-seconds value. **Returns**: `0` on success, or `-EINVAL` if the pointer is null.

`user/newlib_syscalls.c`'s `gettimeofday()` always reports `tv_usec = 0` and never fills in the (obsolete, per POSIX's own advice) `timezone` argument; `time()` is built on top of it.

---

## File descriptors are now shared, refcounted open file descriptions

Before `SYS_PIPE`/`SYS_DUP`/`SYS_DUP2` existed, each `fd_entry_t` slot in a task's fd table *was* the open file — its data buffer, path, and seek offset lived directly in that slot, and `fork()` deep-copied every open fd into the child (giving the child an independent copy — and an independent seek offset — of everything the parent had open, not real POSIX `fork()` semantics).

That changed: `fd_entry_t` (`include/pureunix/task.h`) is now just `{ used; open_file_t *file; }` — a pointer to a separately refcounted `open_file_t` (a real "open file description" in POSIX's sense) drawn from a fixed system-wide pool (`kernel/task.c`'s `g_open_files[128]`; `open_file_alloc()`/`open_file_ref()`/`open_file_unref()`). `fork()` now *shares* the open_file_t (bumping its refcount) instead of copying it — parent and child genuinely share the seek offset afterward, matching real UNIX. `dup()`/`dup2()` share the same way; `close()` (`SYS_CLOSE`) just drops a reference, and the underlying `open_file_t` (and, for a written `FD_KIND_FILE`, its flush to the VFS) only happens once the last reference is gone.

fds 0/1/2 start with `file == NULL`, meaning "the default console binding" (`SYS_READ`/`SYS_WRITE` special-case a null file on fd 0/1/2 to route straight to the tty/VGA driver); `dup2()`ing something onto one of them installs a real `open_file_t` there instead — exactly like a real UNIX process redirecting its own stdin/stdout/stderr — and `close()` on 0/1/2 now succeeds (reverting to the console binding) rather than the old hardcoded `-EBADF`.

---

## SYS_PIPE (36) / SYS_DUP (37) / SYS_DUP2 (38)

A pipe is a fixed 4096-byte ring buffer (`pipe_buf_t`, `include/pureunix/task.h`) shared by two `open_file_t`s — a read end and a write end — created together by one `SYS_PIPE` call. There is no real blocking/wakeup mechanism in this kernel (strictly cooperative scheduling — see `docs/scheduler.md`), so a read on an empty pipe (with the write end still open) or a write to a full pipe (with the read end still open) spins `task_yield()` in a loop until the condition changes — this only accomplishes anything if some other task (typically a forked child on the other end) actually runs in between yields and moves data.

**SYS_PIPE** — **Arguments**: `EBX`: pointer to an `int[2]` to receive the two fds (`[0]` = read end, `[1]` = write end). **Returns**: `0` on success, or `-EINVAL` (null pointer) / `-EMFILE` (fewer than two free fd slots) / `-ENOSPC` (the open-file-description pool, `MAX_OPEN_FILE_DESCRIPTIONS` = 128 system-wide, is full).

A pipe's read end returns `0` (EOF) once its buffer is empty and every write end referencing that pipe has been closed; a write to a pipe with no read ends left returns the errno `-EPIPE` directly — no actual `SIGPIPE` signal gets sent to the writer (the kernel never automatically signals anything on a broken pipe; see `SYS_KILL` below for what signal delivery *does* exist — a caller has to ask for it explicitly).

**SYS_DUP** — **Arguments**: `EBX`: the fd to duplicate. **Returns**: a new fd (the lowest-numbered free slot — see `SYS_OPEN`'s "Descriptor allocation" for what "free" means for 0/1/2 specifically) sharing the same `open_file_t` (including its seek offset) as `oldfd`, or a negative error code (`-EBADF` if `oldfd` isn't open, `-EMFILE` if no fd slot is free).

**SYS_DUP2** — **Arguments**: `EBX`: the fd to duplicate. `ECX`: the fd number the caller wants to become a copy of it. **Returns**: `newfd` on success, or a negative error code (`-EBADF` if either fd is out of range or `oldfd` isn't open). If `newfd` was already open, it's closed first (dropping its own reference, exactly like an explicit `SYS_CLOSE`) before being made an alias for `oldfd`'s description. A no-op (just returns `newfd`) if `oldfd == newfd`.

---

## SYS_KILL (39)

Terminates another task with a signal's POSIX **default action** — there is no handler-dispatch mechanism (that would mean injecting a call frame into a running ring-3 task's own stack and trampolining back once the handler returns, a much larger feature this kernel doesn't have), so every nonzero signal just kills the target outright, unconditionally, regardless of what `sigaction()`/`signal()` was (fictitiously) told to do with it (see `user/newlib_syscalls.c` — both remain no-ops). This is enough for `kill`/`killall`-style process termination and for a `wait()`er to correctly distinguish "exited normally" from "killed by a signal", but not for a shell to catch and react to a signal itself, or for job control (`SIGSTOP`/`SIGTSTP`/`SIGCONT` all take this same generic "kill it" action, not the real stop/continue semantics — there's no `TASK_SLEEPING`-style "stopped" state wired up to them).

**Arguments**: `EBX`: target pid. `ECX`: signal number (0 is POSIX's "null signal" — probes whether `pid` exists and is killable without actually sending anything).

**Returns**: `0` on success. If `pid` is the caller's own, a nonzero signal never returns at all — the kernel stops the caller directly (`task_exit(-sig)`, the same mechanism every other process-termination path uses), same as `_exit()`. For any other `pid`:

| Code | Condition |
|---|---|
| `-EINVAL` | `pid <= 0` — no process-group support (real `kill(2)` treats `pid <= 0` as "send to a process group", which doesn't exist here) |
| `-ESRCH` | no task with that id exists (or it already exited and was reaped) |

A signal-terminated task's exit code (read back via `SYS_WAIT`) is `-signal` — always negative, so a `wait()`er can tell it apart from a normal exit's code, which is always `>= 0` (`kernel/task.c`'s `task_kill()`). `user/newlib_syscalls.c`'s `waitpid()`/`wait()` translate this into the Linux-style `WIFSIGNALED`/`WTERMSIG` encoding (the signal number in the low 7 bits) instead of the usual `WIFEXITED`/`WEXITSTATUS` shifted-exit-code encoding.

The interactive kernel shell's own `kill` builtin (`shell/builtins.c`) sends `SIGTERM` — since every signal takes the same default action here, this is cosmetic (matches conventional shell behavior; there's no way to actually observe the difference from `SIGKILL` without real signal handling).

---

## SYS_FCNTL (40)

Minimal `fcntl()` — just the operations BusyBox's `dd`/ash actually need. There is no close-on-exec model in this kernel at all (`exec()` always inherits every fd — see `task_create_user()`/`task_fork()`), so `F_GETFD`/`F_SETFD` are honest no-ops rather than real bookkeeping.

**Arguments**: `EBX`: fd. `ECX`: command. `EDX`: command-dependent argument.

| Command | Behavior |
|---|---|
| `F_GETFD` | Always returns `0` (no close-on-exec flag exists to report) |
| `F_SETFD` | Always returns `0` (accepted, does nothing) |
| `F_GETFL` | Returns the fd's `open()` flags (`O_WRONLY`/`O_APPEND`/`O_CREAT`/`O_TRUNC` — `include/pureunix/fcntl.h`'s bit layout; `user/newlib_syscalls.c`'s `fcntl()` translates to/from newlib's own layout, same as `open()` does) |
| `F_SETFL` | Overwrites the fd's flags with `EDX` |
| `F_DUPFD` | Duplicates the fd (shares the same `open_file_t`, exactly like `SYS_DUP`) into the lowest free fd number that is `>= EDX` — see `SYS_OPEN`'s "Descriptor allocation" for what "free" means for 0/1/2 specifically |
| `F_DUPFD_CLOEXEC` | Identical to `F_DUPFD` — no close-on-exec model exists here, so there's no behavior to add. **Not just an alias in newlib**: `<fcntl.h>` assigns it a distinct numeric value (14, vs. `F_DUPFD`'s 0), so it has to be recognized as its own command, not merely documented as equivalent — this command going unrecognized (falling through to `-EINVAL`) used to be fatal to BusyBox ash's *entire interactive session* the first time it ran any `>`-redirected command, since ash's `savefd()` calls exactly this on every fd it's about to redirect and treats any `fcntl()` failure other than `EBADF` as fatal to the whole interpreter |

**Returns**: command-dependent value above, or `-EBADF` if `fd` isn't open, `-EMFILE` if `F_DUPFD`/`F_DUPFD_CLOEXEC` finds no free slot, `-EINVAL` for an unrecognized command.

---

## SYS_FTRUNCATE (41)

Truncates (or zero-extends) an already-open fd's in-memory file contents. Since every `FD_KIND_FILE` open file description holds its *entire* contents in memory (`open_file_t.data`/`.size`, flushed to the VFS in one shot on close — see "File descriptors are now shared, refcounted open file descriptions" above), this just reallocates that buffer to the new size, copying over whatever original bytes still fit and zero-filling any newly-added region — no different in effect from a real `ftruncate(2)`, just implemented against this kernel's whole-file-buffered model rather than a block-level one.

**Arguments**: `EBX`: fd. `ECX`: new length (bytes).

**Returns**: `0` on success, or `-EBADF` (fd not open), `-EINVAL` (negative length), `-ENOSPC` (allocation failure). `-EINVAL` also if the fd is a pipe (`FD_KIND_PIPE`) — pipes aren't seekable or truncatable, same restriction `SYS_LSEEK` already enforces.

---

## SYS_GETPPID (42)

Returns the calling task's parent's id (`task_t.parent`, set once at fork/creation time and never updated), or `0` if the task has no parent (the very first task, or the kernel's own bootstrap context).

---

## SYS_PING (43)

Sends one ICMP echo request and waits for the matching reply — a thin syscall wrapper around `net/icmp.c`'s `icmp_ping()`, exposing the kernel's own IPv4/ICMP stack (`docs/networking.md`) to userspace without a general BSD-sockets API, which this kernel doesn't have. `user/ping.c` (installed as `/bin/ping`, a symlink to `ping.elf` — see `tools/mkext2.py`) is the only current consumer.

**Arguments**: `EBX`: destination IPv4 address (`ip4_addr_t`, host byte order — `include/pureunix/inet.h`'s `IP4_ADDR()` convention). `ECX`: timeout in milliseconds. `EDX`: optional (`0`/`NULL` to omit) pointer to a `uint32_t` that receives the round-trip time in milliseconds on success.

**Returns**: `0` on a received reply, `-ETIMEDOUT` if none arrived within the timeout.

Calls `arch_enable_interrupts()` before calling `icmp_ping()`, for the same reason `SYS_READ`'s `tty_read()` path does before its own blocking wait (see "Interrupts stay masked across a whole syscall" below): `int $0x80` is a 32-bit interrupt gate like any other, so it enters with interrupts masked and only restores them on its own `iret`. `icmp_ping()` blocks on `pit_sleep()`, which needs the PIT tick interrupt to actually fire — without re-enabling interrupts first, this hangs the whole kernel solid, identically to the RX-interrupt-context deadlock documented in `net/ip.c`'s `ip_send()` (see `docs/networking.md`'s "Resolved: real network reachability" section for the full story). Found and fixed the same day `SYS_PING` was added, before it ever shipped in a working state.

Identifier/sequence numbers are managed internally (a kernel-global auto-incrementing sequence counter, the calling task's own id as the ICMP identifier) — callers don't control them. Only one `SYS_PING` call can be in flight system-wide at a time, the same single-outstanding-ping limitation `icmp_ping()` itself documents.

---

## SYS_FSTAT (44)

Metadata for an already-open file descriptor, without needing (or re-resolving) a path — the `fstat(2)` counterpart to `SYS_STAT`.

**Arguments**: `EBX`: fd. `ECX`: pointer to a `struct stat` buffer to fill (same layout `SYS_STAT` uses).

**Returns**: `0` on success, or a negative error code:

| Code | Condition |
|---|---|
| `-EINVAL` | null `struct stat *` |
| `-EBADF` | fd outside `[0, MAX_OPEN_FILES)`, not currently open, or (fd ≥ 3) open but with no backing `open_file_t` |

fd 0/1/2 still holding their default console binding report as a character device (`S_IFCHR`), matching `isatty()`. A `FD_KIND_PIPE` fd reports `S_IFIFO`. A `FD_KIND_FILE` fd re-resolves its stored path (`open_file_t.path`, set at `SYS_OPEN` time) for full Unix metadata (uid/gid/nlink/ino/timestamps/blocks), but **`st_size` always comes from the open file description's live in-memory size** (`open_file_t.size`), not a fresh on-disk stat — the correct thing for a writable fd whose buffered writes haven't been flushed to the VFS yet (see `SYS_CLOSE`), and for a read-only fd this already equals the on-disk size loaded at open time.

Added after a real caller broke on `user/newlib_syscalls.c`'s previous `fstat()`, which was a pure-userspace stub that fabricated `st_size=0` for every non-console fd with no kernel query at all. BusyBox ash's own non-interactive script interpreter (`sh script.sh`) sizes its script-reading buffer from an `fstat()` call on the freshly-opened script fd; seeing `st_size=0` for a real, non-empty script made it treat the file as empty and misparse it — observed as an immediate, deterministic `sh: 3: m` (exit code 2) on `sh repro.sh` for *any* non-trivial script, confirmed via kernel-side instrumentation showing the file's actual on-disk bytes were being served correctly and no read() past the initial open() ever even occurred before the error. See `user/newlib_syscalls.c`'s `fstat()` for the fix.

---

## Interrupts stay masked across a whole syscall

Every syscall's `int $0x80` handler stub (`isr128`, `arch/i386/interrupt_stubs.S`) is an ordinary 32-bit interrupt gate: entering it clears `IF`, and nothing restores it until the stub's own `iret` at the very end. This means **any syscall handler that blocks waiting for an interrupt-driven event must explicitly call `arch_enable_interrupts()` first**, or that event's own interrupt (a PIT tick, a keyboard scancode, a NIC's RX/TX completion) can never fire while nested inside the syscall gate, hanging the task (and, since this kernel is single-core and non-preemptive within an interrupt/syscall, effectively the whole system) forever. `SYS_READ`'s console path (`drivers/tty.c`'s `tty_read()`, before its `keyboard_getkey()` wait) and `SYS_PING` above both do this; any future blocking syscall needs the same treatment.

---

## Unimplemented Syscalls

`munmap` (accepted but a plain `free()` — see `mmap()`'s own entry below), `brk`, `signal` (accepted, always a no-op — see `SYS_KILL` above for what *does* work), `setuid`, `setgid`.

(`fstat` was added as `SYS_FSTAT` — see that section above — after a real caller broke on the userspace-only stub that used to stand in for it: BusyBox ash's `sh script.sh` fabricated `st_size=0` for every fd fstat()'d regardless of the real file size, which made ash treat every script file as empty and mis-parse it.)

`mmap()`/`munmap()` (`user/newlib_syscalls.c`) are real but deliberately narrow: only `MAP_ANON|MAP_PRIVATE` with `fd == -1` (an anonymous private scratch buffer — the one case BusyBox's `dd` applet actually needs) is supported, implemented as a thin `malloc()`/`free()` wrapper rather than a real page mapping, since this kernel still has no virtual-memory-mapping syscall of its own; anything else (a real fd, `MAP_SHARED`, ...) fails with `-ENODEV`.

(`mkdir`, `unlink`, `rmdir`, `rename`, `link`, `symlink`, and `readlink` were added in Stage 4 — see `SYS_MKDIR`, `SYS_UNLINK`, `SYS_RMDIR`, `SYS_RENAME`, `SYS_LINK`, `SYS_SYMLINK`, and `SYS_READLINK` above. `fork`, `exec`, and `wait`/`waitpid` were added alongside per-process address spaces — see `SYS_FORK`, `SYS_EXEC`, and `SYS_WAIT` above; `exec` originally took only a bare path, with `argv`/`envp` added later — see `SYS_EXEC` above. `tcgetattr`/`tcsetattr` were added alongside the console termios layer — see `SYS_TCGETATTR`/`SYS_TCSETATTR` above. `ioctl` (just `TIOCGWINSZ`) and `isatty` were added alongside it — see `SYS_IOCTL` above; `isatty` has no syscall of its own, see that section. `chdir`/`getcwd` were added alongside per-task working directories — see `SYS_CHDIR`/`SYS_GETCWD` above. `getuid`/`getgid` were added alongside the BusyBox port — see `SYS_GETUID`/`SYS_GETGID` above; `setuid`/`setgid` remain unimplemented since there's still no login/privilege model to enforce against. `pipe`/`dup`/`dup2` were added alongside the BusyBox port too — see `SYS_PIPE`/`SYS_DUP`/`SYS_DUP2` above. `fcntl`/`ftruncate`/`getppid` and real relative-path resolution — see "Path Resolution" above — were added when BusyBox ash became the default shell, surfacing the first real independent multi-process programs making raw syscalls with relative paths and POSIX assumptions the in-kernel shell/test suites never exercised.)

`ISIG`'s `VINTR`/`VQUIT`/`VSUSP` are recognized by the tty layer (`drivers/tty.c`) but only `VINTR` has an effect (aborting the current read with `-EINTR`) — there is no signal delivery mechanism yet (`kill`/`signal` above are unimplemented stubs), so `VQUIT` and `VSUSP` are accepted but inert, and no process is ever actually killed or stopped by them.

`SYS_CHMOD`/`SYS_CHOWN` are real on EXT2 now; still `-EROFS` on FAT16 — see above.

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
int    pu_exec(const char *path);                               // SYS_EXEC, bare argv/envp
int    pu_execve(const char *path, char *const argv[],
                  char *const envp[]);                          // SYS_EXEC, real argv/envp
int    pu_wait(int pid, int *status);                           // SYS_WAIT
int    pu_tcgetattr(int fd, struct termios *out);               // SYS_TCGETATTR
int    pu_tcsetattr(int fd, int actions, const struct termios *in); // SYS_TCSETATTR
int    pu_ioctl(int fd, int request, void *argp);               // SYS_IOCTL
int    pu_chdir(const char *path);                              // SYS_CHDIR
int    pu_getcwd(char *buf, size_t size);                        // SYS_GETCWD
int    pu_pipe(int fds[2]);                                     // SYS_PIPE
int    pu_dup(int oldfd);                                       // SYS_DUP
int    pu_dup2(int oldfd, int newfd);                            // SYS_DUP2
int    pu_isatty(int fd);           // no syscall — pu_tcgetattr() succeeding
void   pu_exit(int code);           // int $0x81 directly — see docs/scheduler.md's SYS_EXIT note
void   pu_puts(const char *s);                                  // SYS_WRITE to fd 1
void   pu_puti(int value);                                      // integer to decimal on fd 1
size_t pu_strlen(const char *s);                                // no syscall
int    pu_atoi(const char *s);                                  // no syscall
```

`pu_puts` calls `pu_write(1, s, pu_strlen(s))`.
All syscall wrappers forward directly to `syscall3` — no inline assembly beyond the shared `syscall3` helper.
