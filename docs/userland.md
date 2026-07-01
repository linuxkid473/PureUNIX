# Userland

## Overview

PureUNIX supports executing ELF32 binaries loaded from the FAT16 filesystem. User programs run in the same address space as the kernel with no memory protection or privilege separation. The userland infrastructure consists of:

| Component | Source | Purpose |
|---|---|---|
| ELF loader | `kernel/elf.c` | Load and execute ET_EXEC binaries |
| C runtime | `user/crt0.S` | Entry stub, calls `main` |
| libpure | `user/libpure.c` | Syscall wrappers and utility functions |
| User linker script | `user/linker.ld` | Base address 0x400000 |
| Demo programs | `user/hello.c`, `user/calc.c`, `user/viewer.c`, `user/sh.c`, `user/editor.c` | Stubs and functional examples |
| Test programs | `user/opentest.c`, `user/readtest.c`, `user/ext2test.c` | Syscall and filesystem tests |

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

1. Validates `p_vaddr` in the range `[0x400000, 0x700000)`. Segments outside this range are rejected.
2. Copies `p_filesz` bytes from `data + p_offset` to the virtual address via `memcpy`.
3. If `p_memsz > p_filesz` (BSS), the remaining bytes are zeroed with `memset`.

### Execution

After all segments are loaded, `elf_exec` calls the entry point:

```c
int (*entry_fn)(void) = (int (*)(void))ehdr->e_entry;
int result = entry_fn();
```

The call happens in kernel context, on the kernel stack, with kernel privilege. The return value is returned by `elf_exec` to its caller (`exec_external` in the shell).

**No process isolation**: the user program has unrestricted access to kernel memory and hardware.

---

## C Runtime (crt0)

**Source**: `user/crt0.S`

```asm
.section .text
.global _start
_start:
    call main
    ret
```

`_start` is the ELF entry point. It calls `main` and returns. The return from `_start` goes back to the kernel's `entry_fn()` call in `elf_exec`. There is no `exit` syscall invocation from `crt0`.

**Note**: `SYS_EXIT` (syscall 1) does not terminate the task. If a program wants to signal an exit code, it must call it explicitly before returning from `main`. The kernel resumes after `elf_exec` regardless.

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

- No memory isolation between user programs and kernel.
- No user stack separate from the kernel boot stack.
- No dynamic linking; all code must be statically linked.
- No `printf`, `malloc`, `free`, or any standard library in libpure.
- Only one program can run at a time; no fork or exec.
- No signal delivery.
- File I/O is read-only (`O_RDONLY` only); there is no `O_WRONLY` or `O_RDWR` for file descriptors opened via `SYS_OPEN`.
- File descriptors 0, 1, 2 are permanently reserved for stdin/stdout/stderr.
