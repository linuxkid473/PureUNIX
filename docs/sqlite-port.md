# SQLite Port

## Overview

[SQLite](https://www.sqlite.org) 3.53.3 runs natively inside PureUNIX as
`/bin/sqlite3` (the real, unmodified upstream command-line shell) — a real,
ring-3 PureUNIX ELF process linked directly against the real upstream
SQLite core library, not a cross-compiled host tool or an embedded/stripped
build:

```
# sqlite3 /home/test.db
SQLite version 3.53.3 2026-06-26 20:14:12
Enter ".help" for usage hints.
sqlite> CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, val INTEGER);
sqlite> INSERT INTO t(name,val) VALUES('alice',10);
sqlite> SELECT * FROM t;
1|alice|10
sqlite> .exit
```

SQLite is vendored as source under `third_party/sqlite/` (see that
directory's `README.md` for the vendoring rationale — the real upstream
*amalgamation* release, `sqlite3.c`/`shell.c`, byte-for-byte identical to
what `sqlite.org` ships, zero source patches) and built directly by the
top-level Makefile's `SQLite 3.53.3` section, the same "vendor upstream,
compile with our own Makefile rules" pattern `third_party/tcc/` and
`third_party/lua/` already established. This document covers the parts
specific to *this* port: what had to be added at the kernel/newlib layer
(most importantly, real POSIX file locking), the one genuine general
kernel bug this port found and fixed, the install layout, and testing.

---

## Architecture

### One Makefile section, SQLite's own two source files

Unlike Lua (many small source files assembled from a per-file object
list) or TCC (three small source patches), SQLite's official distribution
form for exactly this kind of embedding *is* two flat files —
`sqlite3.c` (the entire core library as one translation unit) and
`shell.c` (the CLI) — so the Makefile section is the simplest of the
three: compile each into `build/user/sqlite/*.o` with a handful of `-D`
flags (see `third_party/sqlite/README.md`'s "Compile-time configuration"
table), then link both against `newlib_crt0`/`newlib_syscalls.o` exactly
like any other `NEWLIB_PROGRAMS`/TinyCC/Lua entry, producing
`build/user/sqlite3.elf`.

### Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `/bin/sqlite3.elf` | The ELF | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF |
| `/bin/sqlite3` → `/bin/sqlite3.elf` | Symlink | Same pattern as `lua`/`luac`/`neatvi`/`ping`/`tty` — makes `sqlite3` resolve as an ordinary `$PATH` command with no `.elf` suffix, matching the task's exact `sqlite3 /home/test.db` invocation |

EXT2-only (like BusyBox/TCC/Lua) — FAT16 isn't part of the primary ash
userland.

---

## Platform work this needed

Everything below was found by actually compiling and running `sqlite3`
inside PureUNIX under QEMU and iterating — including one genuine kernel
bug found only by driving the exact `sqlite3 /home/test.db` → create →
modify → exit → reboot → reopen workflow the task describes, not by
reading source or reasoning about the VFS model in the abstract.

### 1. Real POSIX advisory record locking (`fcntl(F_SETLK/F_GETLK)`) — didn't exist at all

**Symptom**: every SQLite operation — even a single connection with no
concurrent access — would have failed immediately. SQLite's default unix
VFS (`os_unix.c`) unconditionally probes/acquires PENDING/RESERVED/
SHARED/EXCLUSIVE byte-range locks via `fcntl()` on every transaction,
using specific well-known byte offsets (`PENDING_BYTE` ≈ 1 GiB,
`RESERVED_BYTE`, a 510-byte `SHARED` range) as an in-file signaling
mechanism between connections.

**Root cause**: `user/newlib_syscalls.c`'s `fcntl()` only ever implemented
`F_DUPFD`/`F_GETFD`/`F_SETFD`/`F_GETFL`/`F_SETFL` — `F_GETLK`/`F_SETLK`/
`F_SETLKW` weren't handled at all, even though newlib's own
`<sys/_default_fcntl.h>` already declared the command values and
`struct flock`.

**Fix**: a real, new advisory-locking subsystem — `include/pureunix/
flock.h` + `kernel/flock.c` — wired into `SYS_FCNTL` (`arch/i386/
syscall.c`) and `user/newlib_syscalls.c`'s `fcntl()`. Two deliberate
simplifications from textbook POSIX semantics, both explained in
`flock.h`'s own header comment:

- **Keyed by path, not `(device, inode)`**: PureUNIX's `vfs_stat_t` has
  no `st_dev`, and path-keying is indistinguishable from inode-keying for
  every realistic SQLite use (a database file opened by its own path,
  never hardlinked).
- **Keyed by the owning `open_file_t*`, not a real pid**: real POSIX
  locking is scoped per-*process* specifically to handle a single process
  holding many independent fds on one file — SQLite's own `os_unix.c`
  already works around that surprising rule itself with a per-process
  `unixInodeInfo` bookkeeping layer, so it only calls `fcntl()` when a
  real OS-visible state change is needed. Since every PureUNIX `open()`
  produces its own `open_file_t` and a SQLite connection keeps exactly
  one open for a db file's whole lifetime, "owner" and "process"
  coincide for every case this port needs. `open_file_unref()`
  (`kernel/task.c`) releases every lock an `open_file_t` held when its
  last reference goes away — both an explicit `close()` and process-exit
  cleanup already funnel through it.

No blocking path exists at all: SQLite's default build (without the
opt-in `SQLITE_ENABLE_SETLK_TIMEOUT`) never actually issues a blocking
`F_SETLKW` — every lock attempt is non-blocking `F_SETLK`, with
`SQLITE_BUSY` retried entirely at the SQL layer by the busy-handler
callback. `F_SETLKW` is accepted and handled identically to `F_SETLK` for
exactly this reason — deliberately, not as a shortcut.

### 2. `fchmod()`/`fchown()` — didn't exist at all

**Symptom**: `sqlite3.c` failed to link — undefined references to
`fchmod`/`fchown` (referenced unconditionally from `os_unix.c`'s
syscall-override table, `aSyscall[]`, even though they're only actually
*called* when creating a `-journal` file, to copy the main db file's
permissions onto it).

**Root cause**: only the path-based `chmod()`/`chown()` existed; no
fd-based variants did, and PureUNIX's raw syscall ABI had no way to
resolve an fd to its underlying path from user space at all.

**Fix**: two new real syscalls, `SYS_FCHMOD`/`SYS_FCHOWN`
(`include/pureunix/syscall.h`, `arch/i386/syscall.c`) — trivial mirrors
of the existing `SYS_CHMOD`/`SYS_CHOWN` that resolve against the fd's own
`open_file_t.path` (already stored there for the close-time flush) instead
of a caller-supplied path, then call the same `vfs_chmod()`/`vfs_chown()`.

### 3. `getrusage()` — didn't exist at all

**Symptom**: `shell.c` failed to link — `beginTimer()`/`endTimer()` (the
CLI's `.timer on` meta-command, printing per-query CPU time) call
`getrusage(RUSAGE_SELF, ...)` and use `struct rusage`, neither of which
newlib's bare `i686-elf` target ships a header or implementation for.

**Root cause**: same "header present or absent, implementation never
built for this target" gap as `getrlimit()`/`getpriority()` before it.

**Fix**: `struct rusage`/`RUSAGE_SELF` added to `user/newlib_compat/
sys/resource.h` (which already exists for `getrlimit()`/`setrlimit()`),
and a real (honest-stub) `getrusage()` in `user/newlib_syscalls.c` — same
category as `getrlimit()`/`setrlimit()`/`times()`/`sysconf()`: always
reports zeroed CPU time, since PureUNIX's real per-task CPU-tick counter
(`task_t.cpu_ticks`, `arch/i386/pit.c`) is never surfaced through any
syscall yet. `.timer on` now links, runs, and prints `0.000000` rather
than failing to link the whole shell — a real, narrow gap (no accurate
CPU-time reporting for this one diagnostic command), not a crash.

### 4. A genuine, general kernel bug: a writable, non-truncating reopen of an existing file discarded its content

This is the actual hard part of this port, and not a SQLite-specific fix
at all — it's a real, general PureUNIX filesystem bug, previously
invisible because nothing before SQLite ever needed the behavior it got
wrong.

**Symptom**: after a clean `sqlite3 /home/test.db` session — `CREATE
TABLE`, several `INSERT`s, `.exit` — a **second** process opening the
*same still-open-in-memory* database (even a pure, no-writes `SELECT`)
would see an empty, schema-less database (`no such table: t`,
`PRAGMA integrity_check` reporting "ok" for what looked like a
legitimately fresh empty file). Worse: that second process's own
`close()` would then flush a **zero-byte** file back over the real,
correct 8192-byte one, permanently destroying the data — confirmed with
kernel-side tracing showing an errant `ext2_write_file(".../test.db",
size=0)` call right after a completely correct `ext2_read_file()` had
just loaded the real 8192 bytes.

**Root cause**: `SYS_OPEN`'s writable-open path (`arch/i386/syscall.c`)
only ever loaded an existing file's content into the new file
description's in-memory buffer when `O_APPEND` was set. A plain
`O_WRONLY`/`O_RDWR` reopen of an already-existing file — with **no**
`O_APPEND` and, critically, **no `O_TRUNC` either** — silently started
from a completely empty buffer. This was actually a *documented*,
deliberate simplification (`docs/syscalls.md`'s old SYS_OPEN text: "every
non-append write-open also starts from an empty buffer... there is no
`O_RDWR`"), not an oversight — but every writer that existed before
SQLite (ash's `>` redirection, editors' save paths, BusyBox coreutils)
always *wants* a blank slate and always pairs its writable open with an
explicit `O_TRUNC`, so the missing distinction was never exercised.
SQLite's `unixOpen()` always reopens its db file `O_RDWR|O_CREAT` with
**no** `O_TRUNC` — the first thing in this project to actually depend on
a writable reopen preserving prior content instead of discarding it.

**Fix**: `SYS_OPEN`'s writable-open path now loads existing content
whenever `O_TRUNC` is *not* given (an `O_TRUNC` flag was added to the
extracted-flags set alongside the pre-existing `want_write`/`want_creat`/
`want_append`) — real POSIX `open()` semantics: `O_WRONLY`/`O_RDWR` alone
never discard a file's data; only `O_TRUNC` does. This is a genuine
feature addition (real read-modify-write of an existing file now works
with no separate truncating-write step, resolving the "no O_RDWR"
limitation `docs/syscalls.md` used to document), verified with no
regressions:`user/systest.c`'s open/close test was updated to assert the
*correct* behavior (a non-`O_TRUNC` reopen overlays new bytes onto
existing content rather than discarding it, and a separate new check
confirms `O_TRUNC` still genuinely truncates) — see
`docs/syscalls.md`'s SYS_OPEN section for the updated spec.

---

## Testing

Verified interactively in QEMU against the real persistent disk image
(`build/pureunix.iso`, the exact `make iso` deliverable) via the QMP
`send-key` driving technique (`tools/test-persistent-boot.py`'s own
header comment; see also project memory's
`gotcha_pureunix_qemu_headless_testing`). A new permanent regression
test, `tools/test-sqlite-persistence.py`, drives the exact workflow the
task describes end to end, boots the image **twice** (a hard `SIGTERM`
kill of the first QEMU process, then a completely separate second QEMU
process against the same disk image file — a false pass here is
essentially impossible, since the second boot shares no memory with the
first):

- `which sqlite3` / plain `sqlite3 /home/test.db` resolves via `$PATH`
  with no absolute path or setup, matching the task's exact invocation.
- Real `SQLite version 3.53.3 ...` banner from the genuine upstream CLI.
- `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `CREATE INDEX`, and an
  explicit `BEGIN`/`COMMIT` transaction all apply correctly — verified
  with a `SELECT` matching the exact expected post-edit row set.
- `.exit` then a **separate**, freshly-invoked `sqlite3` process
  re-reading the same file (still the same boot) sees identical data —
  proves `close()`'s flush and a fresh `open()` round-trip correctly
  through PureUNIX's whole-file-in-memory VFS.
- The QEMU process is hard-killed (`SIGTERM`, not a clean shutdown) and a
  completely fresh second QEMU process boots the *same disk image file*;
  `sqlite3 -list /home/test.db "SELECT ...;"` (the CLI's non-interactive
  one-shot mode) reports the exact same rows — proof the data reached the
  real, persistent on-disk EXT2 filesystem, not just in-memory state.
- `tools/test-persistent-boot.py` (a plain-file, non-SQLite persistence
  regression) still passes unchanged — that script was itself found to be
  stale (still driving the old first-boot password wizard `kernel/main.c`
  no longer has, the same staleness class `docs/lua-port.md`'s Testing
  section already found and fixed once in `vt-inject-test.py`) and was
  fixed alongside this port since it blocked verifying the baseline.
- Full regression suite re-verified with no *new* regressions:
  `libctest` 45/45, `exectest` 16/16, `dirtest` 14/14, `systest` 343/345
  (up from the documented 339/341 baseline — two net-new checks added
  for the `O_TRUNC` fix above — same 2 known pre-existing console-geometry
  failures, unchanged; see project memory's
  `gotcha_preexisting_systest_failures`).

---

## Known gaps

- **No real multi-process concurrent write safety.** The `fcntl()` locking
  protocol itself is real and correctly enforces mutual exclusion between
  separate `open_file_t`s on the same path, but PureUNIX's filesystem
  model is still whole-file-buffered-in-memory-per-open (`docs/libc.md`),
  not a shared page cache — two genuinely concurrent processes writing
  the same db file would each hold an independent in-memory copy, and one
  process's writes only become visible to another once it `close()`s.
  This doesn't affect the single-connection-at-a-time usage this port
  targets (one `sqlite3` session at a time, exactly the task's workflow),
  but real concurrent multi-connection access is a deeper architectural
  gap, not something this port's locking work alone can fix.
- **No WAL mode** (`SQLITE_OMIT_WAL`) — no shared-memory VFS layer exists.
  The default rollback-journal mode (what every session in this port's
  testing used) is unaffected.
- **No dynamic extensions** (`SQLITE_OMIT_LOAD_EXTENSION`) — no dynamic
  linker exists at all, same as Lua's `require()` of a compiled `.so`.
- **`getrusage()` always reports zero CPU time** — see "Platform work"
  above; only affects the `.timer` diagnostic command's accuracy, not
  correctness of any actual query.
- **No crash-consistency guarantee under a kill *mid-write*.** `fsync()`
  is a real no-op (writes already land in this process's own in-memory
  buffer; the only durable flush point is `close()`) — this port's
  persistence test kills QEMU only *after* a clean `.exit`, matching the
  task's specified workflow, not mid-transaction. A crash between two
  `write()`s within one still-open session is not covered.
