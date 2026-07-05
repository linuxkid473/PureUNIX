# Userland

## Overview

PureUNIX supports executing ELF32 binaries loaded from the FAT16 filesystem. User programs run in ring 3 (CPL 3), on their own stack, in the same page directory as the kernel: the fixed `[0x400000, 0x700000)` load window is the only region marked `PAGE_USER`, so user code cannot touch the rest of kernel memory without faulting. The userland infrastructure consists of:

| Component | Source | Purpose |
|---|---|---|
| ELF loader | `kernel/elf.c` | Load and execute ET_EXEC binaries |
| C runtime | `user/crt0.S` | Entry stub, calls `main` |
| libpure | `user/libpure.c` | Syscall wrappers and utility functions |
| User linker script | `user/linker.ld` | Base address 0x400000 |
| Demo programs | `user/hello.c`, `user/calc.c`, `user/viewer.c`, `user/sh.c`, `user/editor.c` | Stubs and functional examples |
| Test programs | `user/opentest.c`, `user/readtest.c`, `user/ext2test.c`, `user/systest.c`, `user/termiostest.c` | Syscall and filesystem tests |
| Vendored libc | `third_party/newlib`, `user/newlib_syscalls.c`, `user/newlib_crt0.c` | Real C library (printf, malloc, libm, stdio, ...) for programs that opt in — see "A real C library (newlib)" below |

---

## ELF Loader

**Source**: `kernel/elf.c`
**Header**: `include/pureunix/elf.h`

### Validation

`elf_exec(data, size)` validates the binary before loading:

1. Minimum size check: at least `sizeof(Elf32_Ehdr)`.
2. Magic bytes: `\x7fELF` at offset 0.
3. Class: `ELFCLASS32` (32-bit).
4. Type: `ET_EXEC` (executable).
5. Machine: `EM_386` (i386).
6. Program header offset is within `data`.

Returns `-1` on any validation failure. ELF section headers are ignored.

### Segment Loading

For each `PT_LOAD` segment in the program header table:

1. Validates `p_vaddr` in the range `[0x400000, 0x6F0000)` — the top 64 KiB of the `[0x400000, 0x700000)` window is reserved for the user stack. Segments outside this range are rejected.
2. Copies `p_filesz` bytes from `data + p_offset` to the virtual address via `memcpy`.
3. If `p_memsz > p_filesz` (BSS), the remaining bytes are zeroed with `memset`.

### Execution

After all segments are loaded, `elf_exec` marks the whole `[0x400000, 0x700000)` window `PAGE_USER` (see `kernel/vmm.c`), spawns a task via `task_create_user(name, e_entry, 0x700000)`, and blocks in `task_join()` until it exits:

```c
task_t *child = task_create_user("user", entry, USER_WINDOW_END);
return task_join(child);
```

The task's first switch-in runs `enter_usermode()` (`arch/i386/usermode.S`), which `iret`s to CPL 3 at `e_entry` with `ESP` at the top of the 64 KiB user stack. A per-task kernel stack (`arch/i386/gdt.c`'s TSS, updated on every `task_yield()`) is what lets a later trap from ring 3 land back in the kernel safely. The return value is returned by `elf_exec` to its caller (`exec_external` in the shell).

**Still no isolation between user *programs***: every ELF run shares the same fixed window and the same page directory as the kernel (no per-process address space yet), so only the kernel-vs-user boundary is enforced — not process-vs-process.

---

## C Runtime (crt0)

**Source**: `user/crt0.S`

```asm
.section .text
.global _start
_start:
    call main
    movl %eax, %ebx
    int $0x81
.Lhang:
    jmp .Lhang
```

`_start` is the ELF entry point. It calls `main`, then traps to `int $0x81` with the return value in `EBX`. This is **not** the public `SYS_*` syscall gate (`int $0x80`) — it is a separate, kernel-internal vector (`isr129`, wired up in `arch/i386/idt.c`) whose handler calls `task_exit(ebx)`. `task_exit()` never returns to the dying task; it context-switches to whichever task is blocked on it in `task_join()` (normally `elf_exec`'s caller). `.Lhang` is an unreachable safety net.

**Note**: `SYS_EXIT` (syscall 1, reachable via `int $0x80`) is a different thing entirely and still does not terminate the task — it is a documented pass-through that just echoes `EBX` back (see `docs/syscalls.md` and the "SYS_EXIT does not terminate the caller" checks in `user/systest.c`). Actual process termination only happens through the `int $0x81` trap above, which user code never issues directly — only `crt0` does, after `main` returns.

---

## libpure

**Source**: `user/libpure.c`
**Header**: `user/libpure.h`

libpure provides the syscall ABI wrapper and a small set of utility functions. It is compiled into each user binary.

### Syscall Wrapper

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

### API

```c
/* I/O */
int    pu_write(int fd, const char *buf, size_t len);
int    pu_read(int fd, char *buf, size_t len);

/* File operations */
int    pu_open(const char *path, int flags);
int    pu_close(int fd);
int    pu_lseek(int fd, int offset, int whence);
int    pu_stat(const char *path, struct stat *st);

/* Utilities (no syscall) */
void   pu_puts(const char *s);
void   pu_puti(int value);
size_t pu_strlen(const char *s);
int    pu_atoi(const char *s);
```

| Function | Implementation |
|---|---|
| `pu_write(fd, buf, len)` | `syscall3(SYS_WRITE, fd, (int)buf, len)` |
| `pu_read(fd, buf, len)` | `syscall3(SYS_READ, fd, (int)buf, len)` |
| `pu_open(path, flags)` | `syscall3(SYS_OPEN, (int)path, flags, 0)` |
| `pu_close(fd)` | `syscall3(SYS_CLOSE, fd, 0, 0)` |
| `pu_lseek(fd, offset, whence)` | `syscall3(SYS_LSEEK, fd, offset, whence)` |
| `pu_stat(path, st)` | `syscall3(SYS_STAT, (int)path, (int)st, 0)` |
| `pu_puts(s)` | `pu_write(1, s, pu_strlen(s))` |
| `pu_puti(v)` | decimal string conversion, then `pu_write` per digit |
| `pu_strlen(s)` | iterates to null terminator, no syscall |
| `pu_atoi(s)` | iterates digits, no syscall |

`pu_open` resolves paths through the VFS: EXT2 is tried first (files on the data disk), then FAT16 (files in `/bin/` on the program disk). The file's entire contents are buffered in the kernel at open time.

`pu_read(fd, buf, len)` for fd ≥ 3 reads from the kernel buffer allocated at `pu_open` time, advancing the offset. Returns 0 at EOF. No disk I/O occurs during `pu_read`.

`pu_read(0, buf, len)` reads from stdin (keyboard) as before, blocking until Enter.

### `struct stat`

```c
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = file, 2 = directory */
    unsigned short st_attr;   /* FAT attribute byte; 0 for EXT2 entries */
};
```

---

## Linker Script

**Source**: `user/linker.ld`

```
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text   : { *(.text)   }
    .rodata : { *(.rodata) }
    .data   : { *(.data)   }
    .bss    : { *(.bss) *(COMMON) }
}
```

All user programs are linked at base address `0x400000`. The ELF loader enforces that segments fall within `[0x400000, 0x700000)`, giving at most 3 MiB of usable virtual address space per program.

---

## A real C library (newlib)

**Source**: `third_party/newlib` (vendored build — see `third_party/newlib/README.md` and `tools/build-newlib.sh`), `user/newlib_syscalls.c`, `user/newlib_crt0.c`

Programs that need more than libpure's small utility set (`printf`, `malloc`, `qsort`, `fopen`/`fread`/`fwrite`, `<math.h>`, `setjmp`/`longjmp`, ...) can link against a real C library instead: [newlib](https://sourceware.org/newlib/), cross-compiled for the bare `i686-elf` target and vendored into the repo so the build stays network-free.

newlib has no built-in support for a `i686-*-elf` OS, so it never provides POSIX-named syscall wrappers (`open`, `read`, `write`, ...) on its own — it only calls them. The vendored build was configured with `-DMISSING_SYSCALL_NAMES`, which makes newlib's internal reentrant layer call those plain POSIX names directly; `user/newlib_syscalls.c` implements exactly those names (`open`, `close`, `read`, `write`, `lseek`, `fstat`, `stat`, `isatty`, `link`, `unlink`, `mkdir`, `getpid`, `fork`, `execve`, `wait`/`waitpid`, `kill`, `_exit`, `sbrk`, plus `environ`) as thin translations to the raw `int $0x80` ABI (deliberately not through `libpure.h`, to avoid its `struct stat`/`mode_t`/etc. colliding with newlib's own). `sbrk()` bumps a pointer through a static 1 MiB array reserved in the program's own `.bss`, since PureUNIX has no `brk`/`mmap` syscall.

`user/newlib_crt0.c` is the entry stub for newlib-linked programs (`exit(main())` rather than crt0.S's raw `int $0x81`), so `exit()` flushes stdio buffers and runs `atexit()` handlers before the process actually ends.

A program opts into this by being listed in the Makefile's `NEWLIB_PROGRAMS` (currently just `libctest`) rather than `USER_PROGRAMS`; it links `newlib_crt0.o` + `newlib_syscalls.o` + `-lc -lm` instead of `crt0.o` + `libpure.o`. See `user/libctest.c` for the regression suite exercising this libc (`printf`/`scanf`, `string.h`, `stdlib.h`'s heap and `qsort`, `ctype.h`, `libm`, `setjmp`/`longjmp`, and file I/O through `fopen`/`fread`/`fwrite` all the way down to the EXT2-backed VFS).

---

## Programs

### hello.c

Functional. Calls `pu_puts("hello from a PureUnix ELF program\n")` and returns 0.

### calc.c

Demo. Demonstrates `pu_puti`. Uses hardcoded values (prints `12 * 12 = 144`, `144 / 12 = 12`). Does not read user input.

### viewer.c / sh.c / editor.c

Stubs. Print "not yet implemented" messages. The actual shell and editor run as kernel builtins (`vim FILE`, `sh` launches the in-kernel shell, etc.).

### opentest.c

Tests all file-adjacent syscalls — `open`, `stat`, `lseek`, `close` — across success and error paths:

- Open: valid file, missing file, directory, null path, wrong flags.
- Stat: valid file (prints size and type), directory, missing file, null path, null buffer.
- Lseek: `SEEK_SET`, `SEEK_CUR`, `SEEK_END`, negative-result rejection, bad whence.
- Close: valid fd, double-close, lseek-after-close, stdin (fd 0), out-of-range fds.

### readtest.c

Tests `SYS_READ` on VFS-backed file descriptors (fd ≥ 3). Opens a file with `pu_open`, reads it in chunks with `pu_read`, and verifies offset advancement, EOF detection, and partial reads. Verifies that both EXT2 and FAT16 files can be opened and read.

### ext2test.c

14-case EXT2 integration test. Uses `pu_open`, `pu_read`, `pu_stat`, `pu_lseek`, `pu_close`. Covers:

1. Stat root directory (`/`)
2. Stat regular file and verify size
3. Read file content prefix
4. `SEEK_SET` re-read from beginning
5. EOF detection (read after end of file returns 0)
6. Nested directory traversal (`/etc/passwd`, `/home/user/notes.txt`)
7. Non-existent path → `-ENOENT`
8. Directory opened as file → `-EISDIR`
9. `bigfile.bin` (5120 bytes): block-boundary read at 1024-byte mark
10. `hugefile.bin` (14336 bytes): singly-indirect block content verification
11. `SEEK_CUR` navigation
12. Two simultaneous open file descriptors

---

## Building User Programs

User programs are compiled with:

```
i686-elf-gcc -ffreestanding -nostdlib -nostdinc -I user/ \
             -T user/linker.ld \
             user/crt0.S user/libpure.c user/<program>.c \
             -o build/user/<program>.elf
```

Key flags:
- `-ffreestanding`: no standard library assumptions.
- `-nostdlib -nostdinc`: prevents linking against the host libc or using host headers.
- `user/libpure.h` is the only available header for user programs.

The resulting ELF files are placed in `build/user/` and then written into the FAT16 disk image under `/BIN/` by `tools/mkfat16.py`.

---

## Limitations

- User programs run in ring 3 with their own 64 KiB stack, but there is still no per-process address space: every program loads into the same fixed `[0x400000, 0x700000)` window, so two programs' memory is never isolated from each other, only from the kernel.
- Only one program can run at a time (see below), so the single fixed window is never actually contended.
- No dynamic linking; all code must be statically linked.
- No `printf`, `malloc`, `free`, or any standard library in libpure.
- Only one program can run at a time; no fork or exec.
- No signal delivery.
- File I/O is read-only (`O_RDONLY` only); there is no `O_WRONLY` or `O_RDWR` for file descriptors opened via `SYS_OPEN`.
- File descriptors 0, 1, 2 are permanently reserved for stdin/stdout/stderr.
