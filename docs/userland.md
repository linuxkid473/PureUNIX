# Userland

## Overview

PureUnix supports executing ELF32 binaries loaded from the FAT16 filesystem. User programs run in the same address space as the kernel with no memory protection or privilege separation. The userland infrastructure consists of:

| Component | Source | Purpose |
|---|---|---|
| ELF loader | `kernel/elf.c` | Load and execute ET_EXEC binaries |
| C runtime | `user/crt0.S` | Entry stub, calls `main` |
| libpure | `user/libpure.c` | Syscall wrappers and utility functions |
| User linker script | `user/linker.ld` | Base address 0x400000 |
| Demo programs | `user/hello.c`, `user/calc.c`, `user/viewer.c`, `user/sh.c`, `user/editor.c`, `user/opentest.c` | Example ELF programs |

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

Returns `-1` on any validation failure. The ELF section headers are ignored.

### Segment Loading

For each `PT_LOAD` segment in the program header table:

1. Validates `p_vaddr` in the range `[0x400000, 0x700000)`. Segments outside this range are rejected and cause `elf_exec` to return `-1`.
2. Copies `p_filesz` bytes from `data + p_offset` to the virtual address via `memcpy`. Because the kernel uses an identity-mapped flat address space, `p_vaddr` is used directly as a pointer.
3. If `p_memsz > p_filesz` (BSS), the remaining bytes are zeroed with `memset`.

Only `PT_LOAD` segments are processed; all other segment types are silently skipped.

### Execution

After all segments are loaded, `elf_exec` calls the entry point:

```c
int (*entry_fn)(void) = (int (*)(void))ehdr->e_entry;
int result = entry_fn();
```

The call happens in kernel context, on the kernel stack, with kernel privilege. The return value of the entry function is returned by `elf_exec` to its caller (`exec_external` in the shell).

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

`_start` is the ELF entry point. It calls `main` and returns. The return from `_start` returns to the kernel's `entry_fn()` call in `elf_exec`. There is no `exit` syscall invocation from `crt0`.

**Note**: `SYS_EXIT` (syscall 1) does not terminate the task. If a program wants to signal an exit code, it must call it explicitly before returning from `main`. The kernel will resume execution after `elf_exec` regardless.

---

## libpure

**Source**: `user/libpure.c`  
**Header**: `user/libpure.h` (included by user programs directly)

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

All syscalls are invoked through this function. The `"memory"` clobber prevents the compiler from reordering memory accesses across the syscall.

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

`pu_puti` converts using a local 16-byte buffer, building digits in reverse, then writes one digit at a time with `pu_write`.

### `struct stat`

`pu_stat` fills a caller-provided `struct stat`:

```c
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = file, 2 = directory */
    unsigned short st_attr;   /* FAT attribute byte */
};
```

The layout is identical to the kernel-side `struct pureunix_stat` in `include/pureunix/stat.h`. Since programs share the kernel address space, the kernel writes directly into the caller's buffer.

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

## Demo Programs

### hello.c

Functional. Calls `pu_puts("hello from a PureUnix ELF program\n")` and returns 0.

```c
int main(void) {
    pu_puts("hello from a PureUnix ELF program\n");
    return 0;
}
```

### calc.c

Demo stub. Demonstrates `pu_puti`. Uses hardcoded values (prints `12 * 12 = 144`, `144 / 12 = 12`). Does not read user input.

### viewer.c

Stub. Prints:
```
viewer: file viewer not yet implemented
```

### sh.c (user)

Stub. Prints:
```
sh: shell not yet implemented as a standalone ELF
```

This is distinct from the kernel's shell (`shell/sh.c`). Running `/bin/sh.elf` from the shell executes this stub.

### editor.c (user)

Stub. Prints:
```
editor: standalone editor not yet implemented
run nano FILE to edit files
```

Note: the message says `nano`, but the correct command in PureUnix is `vim FILE`. The kernel editor is invoked via the `vim`/`vi` shell builtins, not via this ELF binary.

### opentest.c

Comprehensive file syscall test. Exercises all four file-related syscalls — `open`, `stat`, `lseek`, `close` — across both success and failure paths:

- Open: valid file, missing file, directory, null path, wrong flags.
- Stat: valid file (prints size and type), directory, missing file, null path, null buffer.
- Lseek: `SEEK_SET`, `SEEK_CUR`, `SEEK_END`, negative-result rejection, bad whence.
- Close: valid fd, double-close, lseek-after-close, stdin (fd 0), out-of-range fds.

Each test prints the raw return value so failures are visible without stopping the program.

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
- No stack for user programs separate from the kernel boot stack (ELF programs run on the kernel stack at the point `elf_exec` is called).
- No dynamic linking; all code must be statically linked into the ELF.
- No `printf`, `malloc`, `free`, `string.h`, or any standard library in libpure.
- Only one program can run at a time; there is no fork or exec.
- No signal delivery to user programs.
- File I/O is read-only (`O_RDONLY` only). `SYS_READ` on an open file fd is not yet implemented; `open` + `lseek` manage the offset but reading file data via syscall requires `SYS_READ` extension.
- File descriptors 0, 1, 2 are permanently reserved for stdin/stdout/stderr and cannot be closed.
