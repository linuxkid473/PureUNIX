# Userland

## Overview

PureUNIX supports executing ELF32 binaries loaded from the FAT16 filesystem. User programs run in ring 3 (CPL 3), on their own stack, in the same page directory as the kernel: the fixed `[0x400000, 0x700000)` load window is the only region marked `PAGE_USER`, so user code cannot touch the rest of kernel memory without faulting. The userland infrastructure consists of:

| Component | Source | Purpose |
|---|---|---|
| ELF loader | `kernel/elf.c` | Load and execute ET_EXEC binaries |
| C runtime | `user/crt0.S` | Entry stub, calls `main` |
| libpure | `user/libpure.c` | Syscall wrappers and utility functions |
| User linker script | `user/linker.ld` | Base address 0x400000 |
| Demo programs | `user/hello.c`, `user/calc.c`, `user/viewer.c`, `user/sh.c`, `user/editor.c`, `user/ping.c` | Stubs and functional examples |
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
    movl (%esp), %eax   /* argc, placed by kernel/elf.c's build_argv_stack() */
    movl 4(%esp), %edx  /* argv */
    pushl %edx
    pushl %eax
    call main
    movl %eax, %ebx
    int $0x81
.Lhang:
    jmp .Lhang
```

`_start` is the ELF entry point. `kernel/elf.c`'s `elf_load_into()` builds a POSIX-shaped `argc`/`argv` frame at the top of every new process's stack before it ever runs (see `build_argv_stack()`), so `[esp]`/`[esp+4]` at `_start` are always valid — `_start` just re-pushes them in cdecl order before calling `main`. Programs declared `main(void)` (most of `user/*.c`) simply ignore the two extra stack slots, exactly like a real libc crt0 handles a program that doesn't care about its arguments. `main` then traps to `int $0x81` with the return value in `EBX`. This is **not** the public `SYS_*` syscall gate (`int $0x80`) — it is a separate, kernel-internal vector (`isr129`, wired up in `arch/i386/idt.c`) whose handler calls `task_exit(ebx)`. `task_exit()` never returns to the dying task; it context-switches to whichever task is blocked on it in `task_join()` (normally `elf_exec`'s caller). `.Lhang` is an unreachable safety net.

### Passing argv

`elf_exec_argv(path, argc, argv)` (the argv-aware sibling of `elf_exec(path)`, which just calls it with `argc=1, argv={path, NULL}`) is what the shell (`shell/sh.c`'s `exec_external`) calls with its own already-parsed `cmd->argc`/`cmd->argv`, so a program invoked as `neatvi /test.txt` really does see `argv[1] == "/test.txt"` — there is no `pipe()`/`dup()`/PATH search (see `user/vi/cmd.c`'s header comment for what that rules out), but plain argv passing now works end to end. Before this existed, every program had to declare `main(void)`; a `main(int argc, char *argv[])` would have read uninitialized register/stack garbage as `argc`/`argv` and almost certainly crashed on the first `argv[0]` dereference — exactly what happened when `user/vi/vi.c` (see below) was first ported.

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

newlib has no built-in support for a `i686-*-elf` OS, so it never provides POSIX-named syscall wrappers (`open`, `read`, `write`, ...) on its own — it only calls them. The vendored build was configured with `-DMISSING_SYSCALL_NAMES`, which makes newlib's internal reentrant layer call those plain POSIX names directly; `user/newlib_syscalls.c` implements exactly those names (`open`, `close`, `read`, `write`, `lseek`, `fstat`, `stat`, `isatty`, `link`, `unlink`, `mkdir`, `rmdir`, `chmod`, `chown`, `chdir`, `getcwd`, `opendir`/`readdir`/`closedir`, `getpid`, `fork`, `execve`, `wait`/`waitpid`, `kill`, `_exit`, `sbrk`, plus `environ`) as thin translations to the raw `int $0x80` ABI (deliberately not through `libpure.h`, to avoid its `struct stat`/`mode_t`/etc. colliding with newlib's own). `sbrk()` bumps a pointer through a static 1 MiB array reserved in the program's own `.bss`, since PureUNIX has no `brk`/`mmap` syscall. `execve()` passes `argv`/`envp` straight through to `SYS_EXEC` (see `docs/syscalls.md`'s SYS_EXEC section) — real argument and environment passing across `fork()`+`execve()`, not just a bare path. `wait()`/`waitpid()` also translate PureUNIX's raw exit-code status into the Linux-style `(code << 8)` encoding newlib's `<sys/wait.h>` `WIFEXITED`/`WEXITSTATUS` macros expect (see the same doc's SYS_WAIT section) — the kernel's own status convention is unchanged.

`user/newlib_crt0.S` + `user/newlib_crt0.c` are the entry stub for newlib-linked programs. `newlib_crt0.S`'s `_start` reads the same raw `argc`/`argv`/`envp` frame `kernel/elf.c`'s `build_argv_stack()` places on the initial stack that `user/crt0.S` reads (see "Passing argv" above) — it has to be real assembly for the same reason: no compiler-generated prologue can run before those words are captured, since there's no return address in front of them. It then makes a normal cdecl call into `_start_c(argc, argv, envp)` (`user/newlib_crt0.c`), which sets `environ = envp` and calls `exit(main(argc, argv))` rather than crt0.S's raw `int $0x81`, so `exit()` flushes stdio buffers and runs `atexit()` handlers before the process actually ends.

A program opts into this by being listed in the Makefile's `NEWLIB_PROGRAMS` (currently `libctest`, `exectest`, and `dirtest`) rather than `USER_PROGRAMS`; it links `newlib_crt0_asm.o` + `newlib_crt0.o` + `newlib_syscalls.o` + `-lc -lm` instead of `crt0.o` + `libpure.o`. See `user/libctest.c` for the regression suite exercising this libc (`printf`/`scanf`, `string.h`, `stdlib.h`'s heap and `qsort`, `ctype.h`, `libm`, `setjmp`/`longjmp`, and file I/O through `fopen`/`fread`/`fwrite` all the way down to the EXT2-backed VFS), `user/exectest.c` for the regression suite exercising real `argv`/`envp` passing through `fork()`+`execve()`, and `user/dirtest.c` for the regression suite exercising `opendir()`/`readdir()`/`closedir()` (see below).

Newlib's own `<dirent.h>` (`third_party/newlib/i686-elf/include/sys/dirent.h`) is just the generic "no host support configured" fallback and `#error`s out unconditionally — it was never given a real one for this bare `i686-elf` target. `user/newlib_compat/dirent.h` overrides it (found first because `NEWLIB_CFLAGS` puts `-isystem user/newlib_compat` ahead of `-isystem .../newlib/include` — a single-file, narrower version of the same "compat headers shadow the vendored ones" trick `user/vi/compat/` uses for Neatvi), declaring a real `struct dirent` (`d_name`, `d_type` with standard `DT_UNKNOWN`/`DT_DIR`/`DT_REG`/`DT_LNK` values) and an opaque `DIR` handle. `opendir()`/`readdir()`/`closedir()` (`user/newlib_syscalls.c`) are built over `SYS_READDIR`, which has no cursor of its own — it dumps a whole directory (up to 128 entries) into a caller-supplied array in one call — so `DIR` just buffers that whole dump at `opendir()` time and hands entries out one at a time; there's no true streaming, and a directory with more than 128 entries silently reports only the first 128 (matching `SYS_READDIR`'s own truncation behavior — see `docs/syscalls.md`).

`user/newlib_compat/` has since grown well beyond `dirent.h` — it's the general-purpose "shadow header" mechanism for anything newlib doesn't provide for this bare target (searched first via the same `-isystem` precedence). See "BusyBox" below for what else lives there.

---

## BusyBox

**Source**: `third_party/busybox/busybox.elf` (vendored prebuilt binary — see `third_party/busybox/README.md` and `tools/build-busybox.sh`), linked against the same newlib + `user/newlib_syscalls.c` + `user/newlib_compat/` glue as the `NEWLIB_PROGRAMS` above.

**BusyBox ash is the default login/interactive shell, and BusyBox is the primary userland, as of 2026-07.** A real [BusyBox](https://busybox.net/) 1.36.1 multi-call binary, cross-compiled with BusyBox's own Kbuild. Current applet set (`third_party/busybox/pureunix.config`): `ash`/`sh` (the shell itself — `CONFIG_SH_IS_ASH`), `basename`, `cat`, `chmod`, `chown`, `clear`, `cp`, `dd`, `dirname`, `echo`, `env`, `false`, `find`, `head`, `kill`, `less`, `ln`, `ls`, `mkdir`, `mv`, `printf`, `pwd`, `rm`, `rmdir`, `sleep`, `sort`, `tail`, `tee`, `test`, `touch`, `true`, `wc`, `which`, `xargs`, `yes`. `grep`/`egrep`/`fgrep` and `mount`/`umount`/`ps` are deliberately **not** enabled yet — `grep`/`find -name`'s glob matching works (a real `fnmatch()` was added, see below), but real POSIX `grep`-style regex matching needs `regcomp()`/`regexec()`, which neither newlib nor this kernel has; `mount`/`ps` need a real `mount(2)` syscall and a `/proc` filesystem, neither of which exists yet. Both are tracked as follow-up work (see project memory) rather than shipped half-working. BusyBox's own `vi` is off too — Neatvi ("`user/vi/` (neatvi)" below) is the preferred full-screen editor, installed as a normal `/bin/neatvi` command.

Job control (`CONFIG_ASH_JOB_CONTROL`) is off — there's no process-group/session model or `tcsetpgrp()` in this kernel, so `fg`/`bg`/Ctrl-Z suspend don't exist; plain foreground command execution, `&` background jobs, and pipelines all work via ash's own real `fork()`/`pipe()`/`execve()` calls (this kernel's own primitives, unrelated to the in-kernel shell's historical inability to relay data between two *external* pipeline stages — see `docs/shell.md` — since ash is a genuine multi-process program, not a buffer-relay hack). `CONFIG_ASH_BASH_COMPAT` is off too (its `[[ x =~ pattern ]]` operator needs the same `regcomp()`/`regexec()` gap as `grep`).

### Login Flow

`kernel/main.c`'s `run_login_shell()` execs `/etc/passwd`'s configured shell (`/bin/sh` by default) as a real process right after login — see `docs/shell.md`'s "Login Shell Exec" section for the full mechanism, fallback order (`/bin/puresh`, then the legacy in-kernel shell), and the getty-style "exiting the shell returns you to a fresh login prompt" behavior this introduced.

### Platform Work This Needed

Making BusyBox coreutils and ash behave like a normal, independent multi-process POSIX userland (rather than just a handful of non-forking applets launched by a shell that pre-resolves everything for them) surfaced a real, previously-latent kernel gap: **every path-taking syscall used to hand its raw path straight to the VFS**, which only ever worked because the in-kernel shell always pre-resolved relative paths itself and every regression test always used absolute ones. A bare `ls`/`stat(".")` from a real independent process (ash's own children) exposed this immediately (`ls` failing "No such file or directory" with no arguments) — fixed by resolving every such path against the calling task's own cwd; see `docs/syscalls.md`'s "Path Resolution" section. `kernel/elf.c`'s `elf_load_into()`/`elf_exec_current()`/`elf_exec_argv()` also used to collapse every failure into a generic `-1`, which newlib's errno translation happened to reinterpret as `EPERM` ("Not owner") regardless of the real cause (missing file, bad permissions, bad ELF, OOM) — now returns the real negative errno for each case.

`user/newlib_syscalls.c`'s `getcwd(NULL, 0)` (the glibc auto-malloc extension ash's own `getpwd()` relies on to seed `$PWD` once at startup) was previously unimplemented (passed the `NULL` straight to the kernel, which rejected it) — the visible symptom was `pwd` printing a blank line until the first `cd`.

Beyond that, a long list of smaller platform gaps: `sigaction()`/`sigprocmask()`/`sigsuspend()` (ash always sets up basic signal bookkeeping even with job control off — real signal *delivery* still doesn't exist, see `docs/syscalls.md`'s `SYS_KILL` section, so these just record/report what was asked without anything ever invoking a handler), `getppid()` (new `SYS_GETPPID`), `fcntl()`/`ftruncate()` (new `SYS_FCNTL`/`SYS_FTRUNCATE`, needed by `dd`), `mmap()`/`munmap()` (a narrow anonymous-scratch-buffer-only implementation backed by `malloc()`/`free()`, again for `dd`), `times()`/`sysconf()`/`sched_yield()`/`poll()` (`poll()` is a best-effort "always ready" stand-in — no real readiness multiplexing exists, so `read -t` doesn't genuinely time out yet), real `getpwnam()`/`getpwuid()`/`getgrnam()`/`getgrgid()` (backed by real file I/O against `/etc/passwd` — there's no separate `/etc/group` file, so group identity is resolved by scanning for the account whose gid matches, mirroring the actual one-group-per-user on-disk model), `getrlimit()`/`setrlimit()` (honest "always unlimited, nothing enforced" stubs, just so `ulimit` links and runs), a real `fnmatch()` (glob matching for `find -name`/`-path`), and `ttyname()`/`ttyname_r()` (one console, one name). Every one of these was fixed in the platform layer (`user/newlib_compat/`, `user/newlib_syscalls.c`, new kernel syscalls, `arch/i386/syscall.c`, `kernel/elf.c`) rather than by patching BusyBox's own source, per this project's porting philosophy.

Fitting the resulting, larger `busybox.elf` also ran into a genuine EXT2 limitation — files bigger than ~268 KB (12 direct + 256 singly-indirect blocks, with 1 KB blocks) weren't representable at all. See `docs/developer-guide.md`'s "File Size Cap" section for how doubly-indirect block support was added to both the kernel's read path and `tools/mkext2.py`'s image-builder write path to lift it, rather than continuing to trim applets to fit under the old cap.

Unlike Neatvi (vendored as source and built by this repo's own Makefile rules) or the `NEWLIB_PROGRAMS` (single-file programs compiled directly), BusyBox is vendored as a **prebuilt ELF** — the same choice `third_party/newlib/` makes for the same reason (BusyBox's own source tree is ~4400 files; vendoring it in full would bloat this repository and reintroduce a network dependency at build time). `tools/build-busybox.sh` reproduces it: fetches BusyBox, applies `pureunix.config`, lets BusyBox's own Kbuild compile every applet's object file, then performs the *final link* itself — BusyBox's own link recipe assumes a hosted target with real `crt0.o`/`crt1.o` startup files, which this freestanding cross toolchain doesn't provide, so it fails the same way a naive `i686-elf-gcc -o busybox ...` would for any of this project's own programs without `-nostdlib` and an explicit crt0.

The Makefile just copies the vendored ELF into `$(BUILD)/user/busybox.elf` (`$(BUSYBOX_ELF)`, no compilation) and installs it on the EXT2 image only — `tools/mkext2.py`'s `add_bin()` creates a symlink per entry in its `BUSYBOX_APPLETS` list (`/bin/ls -> busybox.elf`, `/bin/sh -> busybox.elf`, ...), exactly like a real BusyBox install's `/bin/ls -> busybox`; BusyBox dispatches on `argv[0]`'s basename (`applets/applets.c`'s `find_applet_by_name()`). FAT16 has no symlinks, so BusyBox isn't installed there.

---

## TinyCC

**Source**: `third_party/tcc/tcc-0.9.27` (vendored source, unlike newlib/BusyBox's prebuilt binaries — see that directory's `README.md`), built directly by the top-level Makefile against the same newlib + `newlib_syscalls.o` + `newlib_crt0` glue every `NEWLIB_PROGRAMS` entry uses.

[TinyCC](https://bellard.org/tcc/) runs natively as `/bin/tcc` — a real PureUNIX process that itself compiles and links C source into further PureUNIX ELF executables (`tcc hello.c && ./a.out`), not a cross-compiler invoked from the host. See `docs/tcc-port.md` for the full architecture (the two-translation-unit build, the sysroot layout at `/lib/tcc` and `/usr/{include,lib}`, and every platform incompatibility found and fixed while bringing it up — a fixed-size allocator pool exceeding this port's newlib heap, `libtcc1.a`'s path-resolution convention, and `open(O_CREAT, mode)`'s mode argument being silently dropped by the raw syscall ABI, among others) and `docs/libc.md` for the newlib layer TinyCC-compiled programs depend on at runtime, same as any other newlib-linked program.

`/tcctests/` on the EXT2 image holds small permanent regression fixtures (`hello.c`, a `cat`-equivalent, a substring-`grep`-equivalent, and a multi-file program) for smoke-testing `tcc` after any kernel/libc change — see `docs/tcc-port.md`'s "Testing" section.

---

## Programs

### hello.c

Functional. Calls `pu_puts("hello from a PureUnix ELF program\n")` and returns 0.

### calc.c

Demo. Demonstrates `pu_puti`. Uses hardcoded values (prints `12 * 12 = 144`, `144 / 12 = 12`). Does not read user input.

### viewer.c / sh.c / editor.c

Stubs. Print "not yet implemented" messages. The actual shell and editor run as kernel builtins (`vim FILE`, `sh` launches the in-kernel shell, etc.).

### ping.c

Functional. A minimal ICMP echo client — parses a dotted-quad IPv4 address, sends 4 echo requests via `pu_ping()` (`SYS_PING`, a thin wrapper around `net/icmp.c`'s `icmp_ping()` — see `docs/syscalls.md`), and prints per-packet and summary output in a familiar `ping`-like format. Installed as `/bin/ping.elf` with a plain `/bin/ping` symlink (`tools/mkext2.py`'s `add_bin()`, the same "hello -> hello.elf" pattern used elsewhere), so `ping 1.1.1.1` works as an ordinary PATH-searched command. No raw sockets or BSD sockets API involved — this kernel has neither yet; the syscall talks to the kernel's own ICMP client directly.

### user/vi/ (neatvi)

A vendored, close-to-unmodified port of [Neatvi](https://github.com/aligrudi/neatvi), Ali Gholami Rudi's small vi/ex clone — built as `/bin/neatvi.elf` and installed with a plain `/bin/neatvi` symlink (`tools/mkext2.py`'s `add_bin()`, the same "hello -> hello.elf" pattern already used elsewhere), so `neatvi FILE` works as an ordinary PATH-searched command under BusyBox ash too — ash's real `execvp()` only finds actual directory entries, unlike the (now-secondary) in-kernel shell's own external-command dispatch, which always tried appending `.elf` to a bare name itself and so never needed a real symlink for this (deliberately a separate command from the `vi`/`vim` builtins, which still open PureUNIX's own in-kernel modal editor — see `docs/shell.md`). Unlike every other program under `user/`, it's ~20 `.c` files rather than one, so it gets its own object/link rules in the Makefile (`VI_SRCS`/`VI_OBJS`/`VI_CFLAGS`, next to the single-file `%.o`/`%.elf` pattern rules) rather than an entry in `USER_PROGRAMS`.

Upstream Neatvi assumes a POSIX host (`malloc`, `open`/`read`/`write`, a real `<termios.h>`, `fork`+`pipe`+`execvp` for `:!cmd`). `user/vi/compat/` supplies POSIX-named headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `fcntl.h`, `ctype.h`, `signal.h`, `sys/stat.h`, `termios.h`) backed by `user/vi/compat.c`'s implementations (a first-fit `malloc`/`free` over a static 1 MiB arena, `open`/`read`/`write`/... as thin wrappers over `libpure.h`'s `pu_*` syscalls, a fuller `vsnprintf` than the kernel's own since Neatvi uses width/zero-padding it doesn't support, and `tcgetattr`/`tcsetattr` wrappers over `pu_tcgetattr`/`pu_tcsetattr`). `user/vi/term.c` and `user/vi/cmd.c` are the two files with real platform-specific logic (everything else — `vi.c`, `ex.c`, `lbuf.c`, `regex.c`, the Unicode/`uc.c` layer, syntax highlighting, ...  — is unmodified upstream source):

- **`term.c`**: puts the console in raw mode (`ICANON`/`ECHO`/`ISIG` cleared via `tcsetattr`) on `term_init()` and restores it on `term_done()`, exactly like a real vi over a real tty — necessary because PureUNIX's console defaults to canonical+echo (see `drivers/tty.c`), which without this both echoes every keystroke into the screen redraw and delivers input a line at a time instead of a key at a time. Rows/cols are hardcoded to 25×80 (no `ioctl(TIOCGWINSZ)`); `term_suspend()` is a no-op (no job control).
- **`cmd.c`**: `:!cmd`, `!motion`, `:r !cmd`, and `:make` all shell out via `fork()`+`pipe()`+`execvp()` upstream. PureUNIX has `pu_execve()` now (real argv/envp through `SYS_EXEC` — see "A real C library (newlib)" above), but still no `pipe()`, `dup()`, or PATH search, so there's still no way to capture a shelled-out command's output or resolve a bare command name the way `execvp()` does — every function here remains a stub that reports failure, and every call site in `ex.c`/`vi.c` already handles that gracefully (same as a real Neatvi would if `execvp()` itself failed). This is the one Neatvi feature area PureUNIX doesn't support yet.

See [Passing argv](#passing-argv) above for the other missing piece this port needed: without it, `vi.c`'s `main(int argc, char *argv[])` read garbage and paged-faulted the kernel on the very first `argv[0]` dereference.

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
