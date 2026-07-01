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
    unsigned int   st_type;   /* 1 = file, 2 = directory */
    unsigned short st_attr;   /* FAT attribute byte; 0 for EXT2 entries */
};
```

`vfs_stat` tries EXT2 first, then FAT16. The `st_attr` field is 0 for entries resolved through EXT2 (EXT2 mode bits are not surfaced in this field).

---

## Error Return

Any unrecognized syscall number returns `(uint32_t)-1`.

---

## Error Codes

Defined in `include/pureunix/errno.h` (kernel) and `user/libpure.h` (user programs):

| Constant | Value | Meaning |
|---|---|---|
| `ENOENT` | 2 | no such file or directory |
| `EBADF` | 9 | bad file descriptor |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `EMFILE` | 24 | too many open files |

Kernel returns `(uint32_t)-CODE`; user receives a negative `int`.

---

## Unimplemented Syscalls

`fstat`, `mmap`, `munmap`, `brk`, `fork`, `exec`, `wait`, `waitpid`, `kill`, `signal`, `pipe`, `dup`, `dup2`, `chdir`, `getcwd`, `mkdir`, `unlink`, `rename`, `readdir`.

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
void   pu_puts(const char *s);                                  // SYS_WRITE to fd 1
void   pu_puti(int value);                                      // integer to decimal on fd 1
size_t pu_strlen(const char *s);                                // no syscall
int    pu_atoi(const char *s);                                  // no syscall
```

`pu_puts` calls `pu_write(1, s, pu_strlen(s))`.
All syscall wrappers forward directly to `syscall3` — no inline assembly beyond the shared `syscall3` helper.
