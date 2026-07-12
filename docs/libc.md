# C Library

## Overview

PureUNIX has two tiers of C library support for user programs:

| Tier | Source | Used by |
|---|---|---|
| **libpure** | `user/libpure.c`/`user/libpure.h` | Small standalone programs (`hello`, `calc`, `opentest`, ...) — see `docs/userland.md`'s "libpure" |
| **newlib** | `third_party/newlib` (vendored) + `user/newlib_syscalls.c` + `user/newlib_compat/` | Everything else: BusyBox, Neatvi's `compat.c` layer, `libctest`/`exectest`/`dirtest`, and TinyCC (both `tcc.elf` itself and every program `tcc` compiles — see `docs/tcc-port.md`) |

This document covers the newlib tier — the real C library (`printf`,
`malloc`, `<string.h>`, `<math.h>`, `fopen`/`fread`/`fwrite`,
`setjmp`/`longjmp`, POSIX file/process calls, ...) — since it's the one
every program with real dependencies (including anything `tcc` compiles)
relies on, and it's assembled from three layered pieces that weren't
previously documented in one place.

---

## The three layers

### 1. newlib itself (vendored, unmodified)

`third_party/newlib/i686-elf/` — a prebuilt [newlib](https://sourceware.org/newlib/)
cross-compiled for the bare-metal `i686-elf` target with the same
`i686-elf-gcc` used everywhere else in this project. See
`third_party/newlib/README.md` for exactly how it was built
(`tools/build-newlib.sh`) and why it's vendored as a prebuilt binary rather
than built from source on every `make` (network-free, reproducible builds
— the same reasoning `third_party/busybox/` uses).

newlib has no concept of a `i686-*-elf` *operating system* — its own
`configure.host` has no case for one — so it provides every libc
*function* (`printf`, `malloc`, `qsort`, `<math.h>`, ...) but none of the
underlying POSIX *syscall names* (`open`, `read`, `write`, `fork`, ...) a
real OS needs to back them. That's layer 2.

### 2. `user/newlib_syscalls.c` — the POSIX syscall names newlib expects

The vendored newlib build was configured with `-DMISSING_SYSCALL_NAMES`,
which makes its internal reentrant layer (`libc/reent/*.c`) call plain
POSIX names directly (`open`, not `_open`). `user/newlib_syscalls.c`
implements exactly those names, translating each one to `user/libpure.h`'s
raw `int $0x80` syscall ABI (`docs/syscalls.md`):

- **Direct 1:1 translations**: `open`/`close`/`read`/`write`/`lseek`/
  `stat`/`fstat`/`access`/`unlink`/`mkdir`/`rmdir`/`chmod`/`chown`/
  `chdir`/`getcwd`/`link`/`symlink`/`readlink`/`rename`/`fcntl`/
  `ftruncate`/`getpid`/`getppid`/`getuid`/`getgid`/`kill`/`pipe`/`dup`/
  `dup2`/`ioctl`/`tcgetattr`/`tcsetattr`.
- **Real behavior built on a syscall that doesn't map 1:1**:
  - `fork()`/`execve()` — `SYS_FORK`/`SYS_EXEC`, with real `argv`/`envp`
    passing (`docs/syscalls.md`'s `SYS_EXEC` section).
  - `wait()`/`waitpid()` — `SYS_WAIT`, translating PureUNIX's raw exit
    code into the Linux-style `(code << 8)`/`WIFSIGNALED` encoding
    newlib's own `<sys/wait.h>` macros expect.
  - `sbrk()` — bumps a pointer through a fixed-size static array in this
    program's own `.bss` (`NEWLIB_HEAP_SIZE`, currently 1 MiB — see
    `docs/tcc-port.md`'s "Memory budget" for why this size matters and
    what happens when a program's own fixed allocations exceed it),
    since PureUNIX has no `brk`/real virtual-memory syscall at all.
  - `mmap()`/`munmap()` — narrow: only `MAP_ANON|MAP_PRIVATE` with
    `fd == -1` (an anonymous scratch buffer) is supported, implemented as
    a thin `malloc()`/`free()` wrapper. Anything else fails `ENODEV`.
  - `mprotect()` — an honest no-op returning success: PureUNIX's VMM never
    marks a user page non-writable/non-executable at all (no NX-bit
    support, see `docs/memory.md`), so every byte a process can address
    is already simultaneously read/write/exec — there's no real
    protection state to change. Added for TinyCC's `-run` support code;
    see `docs/tcc-port.md`.
  - `getcwd(NULL, 0)` — the glibc auto-malloc extension ash's `getpwd()`
    needs; PureUNIX's own `SYS_GETCWD` requires a real caller-supplied
    buffer, so this allocates one here before making the syscall.
  - `opendir()`/`readdir()`/`closedir()` — built over `SYS_READDIR`, which
    has no cursor of its own (dumps up to 128 entries in one call); `DIR`
    buffers that whole dump at `opendir()` time.
  - `getpwnam()`/`getpwuid()`/`getgrnam()`/`getgrgid()` — real file I/O
    against `/etc/passwd` (no separate `/etc/group` file — group lookups
    scan for the account whose gid matches, mirroring the actual
    one-group-per-user on-disk model).
  - `fnmatch()` — a real glob-matching implementation (needed by BusyBox's
    `find -name`/`-path`), since newlib ships the header but not the
    implementation for this target.
  - `system()`/`popen()`/`pclose()` — real, added for the Lua port
    (`docs/lua-port.md`): the vendored `libc.a`'s own `system()` was an
    unconditional `errno=ENOSYS` dummy, and `popen()`/`pclose()` didn't
    exist at all. All three now spawn a real `/bin/sh -c "..."` child via
    `fork()`/`pipe()`/`dup2()`/`execve()`/`waitpid()` — genuine child
    processes, not stubs.
  - `isatty()` — real, not a guess: issues a `TIOCGWINSZ` `ioctl()` probe
    and lets `arch/i386/syscall.c`'s `tty_fd_check()` (which already
    distinguishes a real console binding from a redirected-to-file/pipe
    fd) answer, instead of the previous `fd >= 0 && fd <= 2` shortcut that
    was wrong for a redirected fd 0/1/2. Found via the Lua port
    (`docs/lua-port.md`) but a generic correctness fix.
  - `flockfile()`/`funlockfile()`/`ftrylockfile()` — real no-ops: newlib
    declares these but never built them for this single-task,
    cooperatively-scheduled target, so there's genuinely nothing for a
    per-`FILE*` lock to exclude.
  - `fcntl(F_GETLK/F_SETLK/F_SETLKW)` — real, added for the SQLite port
    (`docs/sqlite-port.md`): SQLite's default unix VFS depends on genuine
    POSIX advisory record locking for every transaction. Marshals
    newlib's `struct flock` across `SYS_FCNTL` to `kernel/flock.c`'s real
    per-path advisory lock table (see `include/pureunix/flock.h`).
    `F_SETLKW` is handled identically to `F_SETLK` (non-blocking) since
    SQLite's default build never actually issues a blocking lock request.
  - `fchmod()`/`fchown()` — real, added for the SQLite port: didn't exist
    at all before (only the path-based `chmod()`/`chown()` did). New
    `SYS_FCHMOD`/`SYS_FCHOWN` syscalls resolve against the fd's own
    `open_file_t.path` and call the same `vfs_chmod()`/`vfs_chown()` the
    path-based versions use — SQLite's `os_unix.c` uses these to copy a
    database file's permissions onto a freshly created rollback-journal
    file.
- **Honest stubs (accepted, no enforcement model exists to back them)**:
  `sigaction()`/`sigprocmask()`/`sigsuspend()`/`signal()` (record/report
  only — no real signal delivery, see `docs/syscalls.md`'s `SYS_KILL`),
  `getrlimit()`/`setrlimit()` (always "unlimited"), `poll()` (best-effort
  "always ready" — no real readiness multiplexing), `times()`/`sysconf()`/
  `sched_yield()`, `ttyname()`/`ttyname_r()` (one console, one name),
  `getrusage()` (always zeroed CPU time — added for the SQLite port's
  shell `.timer` command; PureUNIX tracks per-task CPU ticks internally,
  `task_t.cpu_ticks`, but nothing surfaces them through a syscall yet).

### 3. `user/newlib_compat/` — shadow headers

Some headers newlib never shipped for a generic bare-metal target at all
(`sys/mman.h`, `sys/wait.h`, `sys/utsname.h`, `dirent.h`'s real struct
instead of the "no host support configured" `#error` fallback, ...), and a
few of newlib's own headers are missing individual declarations real
programs need (`signal.h`'s `SA_RESTART` and friends, `setjmp.h`'s
`_setjmp`/`_longjmp` macros needed by Lua's `LUA_USE_POSIX` build — see
`docs/lua-port.md`). `user/newlib_compat/`
holds PureUNIX's own versions of these, found *first* on the include path
(`NEWLIB_CFLAGS := -isystem user/newlib_compat -isystem $(NEWLIB_DIR)/include`
in the Makefile) so they shadow newlib's own without needing to patch
newlib itself. Files that only add to what newlib already declares use
`#include_next` to fall through to the real header afterward (see e.g.
`user/newlib_compat/signal.h`'s own header comment).

This exact three-tier precedence (own headers → compat shadows → real
newlib) is also what TinyCC's own sysroot reproduces target-side via
`CONFIG_TCC_SYSINCLUDEPATHS` — see `docs/tcc-port.md`.

---

## Building a newlib-linked program

A program opts in by being listed in the Makefile's `NEWLIB_PROGRAMS`
(or, for TinyCC specifically, its own dedicated rule — `docs/tcc-port.md`)
rather than `USER_PROGRAMS`. It links `newlib_crt0_asm.o` + `newlib_crt0.o`
(the entry stub — see `docs/userland.md`'s "A real C library (newlib)")
\+ `newlib_syscalls.o` + `-lc -lm` instead of `crt0.o` + `libpure.o`. See
`docs/userland.md` for the exact link line and `user/libctest.c` for the
regression suite exercising the library itself (`printf`/`scanf`,
`<string.h>`, `<stdlib.h>`'s heap and `qsort`, `<ctype.h>`, libm,
`setjmp`/`longjmp`, file I/O through `fopen`/`fread`/`fwrite`).

---

## Known limitations

- **`sbrk()`'s fixed 1 MiB heap** is shared by every newlib-linked program
  (a single `newlib_syscalls.c` compiled into each); a program with large
  fixed allocations of its own (TinyCC's tal pools were one real example —
  see `docs/tcc-port.md`) can exhaust it before doing any real work. There
  is no per-program heap sizing mechanism yet.
- **No `brk`/real virtual memory.** `sbrk()`/`mmap()` are both narrow
  translations over static arrays or `malloc()`, not real page mappings.
- **No signal delivery**, despite `sigaction()`/`signal()` existing — see
  `docs/syscalls.md`'s `SYS_KILL` section for what *does* work (unconditional
  process termination, not handler dispatch).
- **`open(..., O_CREAT, mode)`'s `mode` is applied via a follow-up
  `SYS_CHMOD`, not atomically at creation** — PureUNIX's raw `SYS_OPEN` has
  no mode argument at all. See `docs/tcc-port.md`'s incompatibility #4 for
  the full story (found while porting TinyCC, whose own compiled output
  needs the execute bit to actually run).
