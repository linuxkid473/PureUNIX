# Lua Port

## Overview

[Lua](https://www.lua.org) 5.4.7 runs natively inside PureUNIX as `/bin/lua`
(the real, unmodified upstream interactive REPL and script runner) and
`/bin/luac` (the standalone bytecode compiler) — real, ring-3 PureUNIX ELF
processes, not a cross-compiled host tool or an embedded/stripped-down
build:

```
$ lua
Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio
> print("hello from PureUNIX")
hello from PureUNIX
> os.execute("ls /bin | head -3")
...
> local f = io.open("/root/notes.txt", "w")
> f:write("real file I/O\n"); f:close()
```

Lua is vendored as source under `third_party/lua/` (see that directory's
`README.md` for the vendoring rationale — byte-for-byte identical to the
upstream release tarball, zero source patches) and built directly by the
top-level Makefile's `Lua 5.4.7` section, the same "vendor upstream,
compile with our own Makefile rules, no upstream build system" pattern
`third_party/tcc/` (TinyCC) already established. This document covers the
parts specific to *this* port: what had to be added at the newlib/kernel
layer, every incompatibility found while bringing it up, the install
layout, and the memory budget. For why no Lua source patches were needed
(unlike TCC's three small `TCC_PUREUNIX`-gated patches), see
`third_party/lua/README.md`.

---

## Architecture

### One Makefile section, Lua's own object lists

Lua's own `src/Makefile` `PLATS` targets (`linux`, `macosx`, `posix`, ...)
each assume a specific hosted toolchain with no PureUNIX case to add
without patching the Makefile itself. Instead, the top-level Makefile's
`Lua 5.4.7` section compiles Lua's own `CORE_O`/`LIB_O` object lists
(`lapi.c`, `lcode.c`, ..., `linit.c` — copied verbatim from Lua's own
`src/Makefile` so this stays a faithful reproduction) into
`build/user/lua/*.o`, then links two ELFs against `newlib_crt0`/
`newlib_syscalls.o` exactly like any other `NEWLIB_PROGRAMS`/TinyCC entry:

- **`lua.elf`** — `lua.o` + `CORE_O` + `LIB_O`
- **`luac.elf`** — `luac.o` + `CORE_O` + `LIB_O` (matches Lua's own
  Makefile, which links `luac` against the full library too, even though
  it only actually calls into the core + `lauxlib`)

### The one `-D` flag standing in for Lua's own `PLAT=posix`

| Define | Purpose |
|---|---|
| `LUA_USE_POSIX` | Takes `luaconf.h` down Lua's already-existing POSIX configuration branch — see below |
| `LUA_COMPAT_5_3` | Carried over from Lua's own Makefile default (harmless 5.3 compatibility shims) |

`LUA_USE_DLOPEN` is deliberately **not** defined — PureUNIX has no dynamic
linker at all (same reasoning as TinyCC's `CONFIG_TCC_STATIC`), so
`loadlib.c` compiles its own upstream "Fallback for other systems" branch:
a complete, real implementation (not a patch) that makes
`package.loadlib`/`require` of a compiled `.so` fail cleanly with
`"dynamic libraries not enabled"`. Pure-Lua module loading is unaffected.

### Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `/bin/lua`, `/bin/luac` | The two ELFs | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF (neatvi, tcc, ping, ...) |
| `/usr/local/share/lua/5.4/` | Pure-Lua modules (`LUA_LDIR`) | Lua's own unmodified default for a non-Windows POSIX build (`luaconf.h`'s `LUA_ROOT "/usr/local/"`) — an ordinary Unix install layout, not a PureUNIX-specific path |
| `/usr/local/lib/lua/5.4/` | C modules (`LUA_CDIR`) | Same — always empty in practice since there's no dynamic linker to load a `.so` from here, but real and on `package.cpath` regardless |
| `/usr/local/share/lua/5.4/greet.lua` | A real, tiny seed module | `tools/mkext2.py`'s `add_lua()` — proves `require()`'s on-disk `package.path` search end to end just by booting the image, not only by a test written after the fact |

All of this is EXT2-only (like BusyBox/TCC) — FAT16 has no symlinks and
isn't part of the primary ash userland.

---

## Platform work this needed

Everything below was found by actually compiling and running `lua`/`luac`
inside PureUNIX under QEMU and iterating, not inferred from reading
source. Each is a real, clean fix at the layer this project's own
conventions call for (a `user/newlib_compat/` shadow header or a real
`user/newlib_syscalls.c` addition), not a Lua source patch or a stub.

### 1. `_setjmp`/`_longjmp`: undeclared, not just unimplemented

**Symptom**: `ldo.c` failed to compile — `_longjmp`/`_setjmp` implicit
declarations.

**Root cause**: `ldo.c`'s own exception-handling macros
(`LUAI_THROW`/`LUAI_TRY`) pick the BSD `_setjmp`/`_longjmp` variants
whenever `LUA_USE_POSIX` is defined ("in POSIX, try _longjmp/_setjmp (more
efficient)", Lua's own comment) — they skip saving/restoring the
process's signal mask, unlike plain `setjmp`/`longjmp`. This newlib was
never configured with a case that declares them (normally gated behind
host-specific macros nothing here defines).

**Fix**: new `user/newlib_compat/setjmp.h` (`#include_next`s newlib's real
`<setjmp.h>`, then `#define`s `_setjmp(env)`/`_longjmp(env,val)` as plain
`setjmp`/`longjmp`). This is a correct implementation, not an
approximation: PureUNIX has no signal mask to skip in the first place —
`sigaction()`/`sigprocmask()` (`user/newlib_syscalls.c`) are honest
bookkeeping only, since no signal ever actually interrupts a running task
to begin with — so the two variants are behaviorally identical here.

### 2. `flockfile`/`funlockfile`: declared, never implemented

**Symptom**: `liolib.o` failed to link — undefined `flockfile`/
`funlockfile` (`l_lockfile`/`l_unlockfile`, `LUA_USE_POSIX`).

**Root cause**: newlib's `<stdio.h>` declares these (`getc_unlocked()`'s
locking companions) but this build was never configured with a threading
model, so the reentrant-locking half of stdio was never compiled into
`libc.a`.

**Fix**: real (not stub) no-op implementations in `user/newlib_syscalls.c`
— genuinely correct, not a placeholder, since PureUNIX is single-task/
cooperatively-scheduled with nothing for a per-`FILE*` lock to exclude.
`ftrylockfile()` added alongside for completeness.

### 3. `system()`: present, but an unconditional dummy

**Symptom**: `os.execute()` always failed.

**Root cause**: the vendored `libc.a`'s own `system()` just sets
`errno = ENOSYS` and returns -1 — no host support was ever configured for
this bare target, even though real `fork()`/`execve()`/`waitpid()` already
exist and work.

**Fix**: a real `system()` in `user/newlib_syscalls.c` — `fork()` + exec
`/bin/sh -c "..."` + `waitpid()`, using the already-correct
`encode_wait_status()` translation so `os.execute()`'s
`true/exit/0`-shaped return value (`lauxlib.c`'s `WIFEXITED`/
`WEXITSTATUS`) is accurate. Defining `system` in an object linked directly
(ahead of `-lc`) wins over the archive's dummy purely through ordinary
archive-member laziness — the same mechanism `dirname()`/`realpath()`/
`fnmatch()` already relied on for functions newlib declared but never
implemented.

### 4. `popen()`/`pclose()`: didn't exist at all

**Symptom**: `io.popen()` had no implementation to link against.

**Root cause**: no host `popen.c` was ever built into this newlib for the
same reason as #3.

**Fix**: real `popen()`/`pclose()` in `user/newlib_syscalls.c` — `pipe()` +
`fork()` + `dup2()` + `execve("/bin/sh", {"sh","-c",cmd})` + `fdopen()`,
with a small fixed pid-tracking table (16 slots, matching this codebase's
general "fixed pool, not `kmalloc`'d" style) so `pclose()` waits on the
right child. Verified end to end: `io.popen("echo ...")` reads real output
back from a real BusyBox ash child process.

### 5. `isatty()`: always answered "yes" for fd 0/1/2, redirected or not

**Symptom**: not a Lua-specific bug, but directly affects
`lua_stdin_is_tty()` (`lua.c`, `LUA_USE_POSIX`) — the REPL would print `>`
prompts even when reading from a redirected file, since the pre-existing
`isatty()` was just `fd >= 0 && fd <= 2`.

**Root cause**: never actually asked the kernel; guessed from the fd
number alone.

**Fix**: `isatty()` now issues the same `TIOCGWINSZ` `ioctl()` probe a real
libc's `isatty()` would, and lets the kernel's existing
`tty_fd_check()` (`arch/i386/syscall.c`) — which already correctly
distinguishes a genuine console binding from a redirected-to-file/pipe fd,
returning `-ENOTTY` for the latter — answer for real. This is a generic
correctness fix (affects every program, not just Lua), found because Lua
was the first thing to actually depend on the distinction.

### 6. `/root` and `/home/guest`: named in `/etc/passwd`, never created

**Symptom**: not a Lua bug either, but blocked realistic testing —
`lua`-authored scripts writing to `"$HOME/..."` failed with "nonexistent
directory" the moment testing tried to use the shell's actual home
directory.

**Root cause**: `kernel/main.c`'s auto-login sets `$HOME=/root` and calls
`shell_set_home_cwd("/root")` on every boot (and `/etc/passwd` has always
named `/root`/`/home/guest` as the two accounts' homes), but
`tools/mkext2.py` never actually created either directory —
`shell_set_home_cwd()` silently no-ops when `vfs_stat()` doesn't find a
real directory, so ash was actually starting in `/`, not `/root`, on every
boot, contradicting both `$HOME` and the password database.

**Fix**: `tools/mkext2.py` now creates both `/root` and `/home/guest`
alongside the pre-existing `/home/user`. A pre-existing gap, unrelated to
Lua specifically, but fixed here since it directly blocked realistic
"boot and just use it" testing of the Lua port.

### 7. `/tmp` didn't exist

Not a failure so much as a latent gap: newlib's `P_tmpdir` (`<stdio.h>`)
is `"/tmp"`, used by `tmpnam()`/`os.tmpname()`/`io.tmpfile()`, and nothing
had ever created it. `tools/mkext2.py`'s new `add_lua()` creates it
alongside Lua's own module directories, since it was the first thing in
this session to actually depend on it existing.

---

## Testing

Verified interactively in QEMU against the real persistent disk image
(`build/pureunix.iso`, the exact `make iso` deliverable) via a headless
QMP `send-key` driving script (see `gotcha_pureunix_qemu_headless_testing`
in project memory — no usable stdin over `-serial stdio`, PS/2-only
keyboard):

- `which lua` resolves via `$PATH` to `/bin/lua`; `lua -v` prints the real
  `Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio` banner.
- `lua -e 'print(1+2, "hi".." there", #({1,2,3}))'` → `3	hi there	3`
  (arithmetic, string concatenation, table length all correct).
- Real file I/O: `io.open(path, "w")`/`write()`/`close()` then
  `io.open(path, "r")`/`read("a")` round-trips correctly through the real
  EXT2 filesystem.
- `os.time() > 0` is true; `os.execute("true")` returns the correct
  `true, "exit", 0` (real `system()`, correct `WIFEXITED` decoding).
- `io.popen("echo from-popen"):read("l")` returns `"from-popen"` — a real
  child process, not a stub.
- `require("greet")` loads the real on-disk seed module from
  `/usr/local/share/lua/5.4/greet.lua` and calls a function in it.
- `luac -o out.luac in.lua` compiles real bytecode; `lua out.luac` runs it
  and produces output identical to running the source directly.
- **Persistence**: a file written by a running `lua` process survives a
  hard-killed QEMU process (SIGTERM, not a clean shutdown) and a
  completely fresh second QEMU process booting the same disk image file —
  proof the write actually reached the on-disk EXT2 filesystem, not just
  in-memory state.
- Full regression suite re-verified with no regressions after all
  `user/newlib_syscalls.c` changes: `libctest` 45/45, `exectest` 16/16,
  `dirtest` 14/14, `systest` 339/341 (2 known pre-existing failures,
  unchanged from baseline — see project memory's
  `gotcha_preexisting_systest_failures`).

`tools/vt-inject-test.py`'s `boot_to_shell()` was also updated in passing:
it still expected the old first-boot password wizard/login-prompt flow,
which `kernel/main.c` no longer has (it now auto-logs in as root
unconditionally) — a pre-existing test-infra staleness unrelated to Lua,
but it blocked running `make run-test` at all until fixed.

---

## Memory budget

Same fixed 3 MiB user address window as every other newlib program
(`docs/tcc-port.md`'s "Memory budget" — `USER_WINDOW_BASE`/`USER_WINDOW_END`,
`include/pureunix/vmm.h`), minus a 64 KiB stack. `lua.elf` uses ~380 KiB
text + ~1.03 MiB data/bss (almost entirely the 1 MiB newlib heap array,
same `NEWLIB_HEAP_SIZE` every `NEWLIB_PROGRAMS`/TinyCC/BusyBox entry
shares) — comfortable headroom for interactive use and small-to-medium
scripts, well within the architectural ceiling.

---

## Known gaps

- **No dynamic modules.** `require()` of a compiled `.so` C extension
  fails cleanly (`loadlib.c`'s own upstream message) — PureUNIX has no
  dynamic linker at all. Pure-Lua modules via `package.path` work fully.
- **Ctrl-C during script execution doesn't actually break out of a
  running Lua loop.** `lua.c`'s `LUA_USE_POSIX` branch installs a real
  `sigaction()` handler for this, but PureUNIX has no real asynchronous
  signal delivery to begin with (a project-wide, pre-existing scope
  boundary — see `docs/syscalls.md`) — `sigaction()` is honest bookkeeping
  only. Ctrl-C at the interactive prompt itself (between commands) still
  works via the pre-existing `VINTR`-driven `EINTR` on a blocked `read()`,
  same as ash.
- **`os.setlocale`** only ever has the "C" locale to offer (no locale data
  is installed anywhere in this userland) — this is a generic newlib/
  BusyBox-wide limitation, not Lua-specific.
