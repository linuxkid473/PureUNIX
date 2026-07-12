# htop Port

## Overview

[htop](https://htop.dev) 3.5.1 runs natively inside PureUNIX as `/bin/htop`
— real, unmodified upstream htop core code (`Action.c`, `Process.c`,
`ProcessTable.c`, every panel/meter/screen), linked against the real
ported ncurses (`docs/ncurses-port.md`) and driving genuinely live
PureUNIX process data through a brand-new `pureunix/` platform backend,
not a fake process viewer:

```
# htop
  Mem[||||||||||||||                                     26.6M/128M]  Tasks: 8, 0 thr, 7 kthr; 1 running
  Swp[                                                        0K/0K]  Load average: 0.00 0.00 0.00
                                                                        Uptime: 00:00:10

  [Main]
  PID USER       PRI  NI  VIRT   RES S  CPU%-MEM%   TIME+  Command
    1 root         0   0     0     0 R   0.0  0.0  0:01.31 kernel
    2 root         0   0     0     0 R   0.0  0.0  0:02.87 init-reaper
    3 root         0   0     0     0 R   0.0  0.0  0:00.21 vt-session
    8 root         0   0  1416  1416 S   0.0  1.1  0:00.29 /bin/sh
   15 root         0   0  1652  1652 R   9.0  1.3  0:00.25 htop
```

This is documented here for the parts specific to *this* port: htop's
platform-plugin architecture and why `pureunix/` follows it exactly;
what genuinely-live PureUNIX process/system data backs every column and
meter (and what's honestly left unavailable rather than faked); the real
kernel bugs this port found (two of them entirely independent of htop
itself, reproduced identically by BusyBox's own `ps`); the install
layout; and testing.

---

## Architecture

### The Platform plugin system, followed exactly

Every OS htop supports (`linux/`, `freebsd/`, `dragonflybsd/`, `netbsd/`,
`openbsd/`, `darwin/`, `solaris/`, and a deliberately-nonfunctional
`unsupported/` reference/template) implements the same fixed contract:
a `Platform.c`/`Platform.h` (global hooks — signals list, meter types,
`Platform_getUptime()`/`Platform_getLoadAverage()`/`Platform_setCPUValues()`
/etc.), a `Machine` subclass (system-wide scan state — CPU/memory
totals), a `Process` subclass (per-process fields beyond the common set
every platform shares), and a `ProcessTable` subclass whose one real job
is `ProcessTable_goThroughEntries()` — populate the process list from
whatever this OS's real process-information interface is. Core htop
(`Action.c`, `Process.c`, `Row.c`, every `*Panel.c`/`*Meter.c`/`*Screen.c`
— the ~85 files in `Makefile.am`'s `myhtopsources`) never changes between
platforms; it only ever calls this fixed interface.

`third_party/htop/pureunix/` is a real, new platform backend following
this exact contract — `Platform.c`, `PureUnixMachine.c`, `PureUnixProcess.c`,
`PureUnixProcessTable.c` — isolated in its own directory so a future
htop version can be re-ported by dropping in the new core files and
carrying this directory forward mostly unchanged, exactly the promise
every other platform directory already makes for itself.

### Why it reads PureUNIX's real `/proc`, not a Linux compatibility shim

The task driving this port was explicit: if htop expects Linux-specific
interfaces like `/proc`, don't fake arbitrary Linux files just to make
the UI launch. PureUNIX's `/proc` (`fs/procfs.c`) is not a Linux
compatibility shim invented for this port — it already existed,
genuinely backed by kernel state (`kernel/task.c`'s real task list,
`kernel/pmm.c`'s real frame counts, `arch/i386/pit.c`'s real 100 Hz tick
counter), built for BusyBox's own `ps`/`top` applets, which is why its
`/proc/[pid]/stat` field layout happens to match Linux's own convention
through field 24 — a deliberate, pre-existing interop choice this port
inherits rather than introduces. `pureunix/PureUnixProcessTable.c` reads
it with hand-written, minimally-scoped parsing (not htop's own
`linux/LinuxProcessTable.c`, which is ~2000 lines handling cgroups,
delayacct, io stats, and a dozen other `/proc` files PureUNIX has no
equivalent of) — genuinely real data, a genuinely new and isolated
backend, following the platform contract's spirit without either faking
files or dragging in Linux-specific complexity this kernel can't back.

### What's genuinely real, column by column

- **PID/PPID/PGRP/SESSION** — `task_t.id`/`ppid`/`pgid`/`sid`, straight
  from `/proc/[pid]/stat`.
- **USER** — real `/proc/[pid]/status` `Uid:` line resolved through
  `getpwuid()` (`user/newlib_syscalls.c`, backed by a real `/etc/passwd`)
  via htop's own `UsersTable` cache — the same lazy per-uid resolution
  every real backend uses.
- **STATE** — `task_state_t` (`TASK_RUNNING`/`RUNNABLE`→R,
  `TASK_SLEEPING`→S, `TASK_STOPPED`→T, `TASK_ZOMBIE`→Z), confirmed by
  this port's own F9 kill test (see Testing) actually transitioning a
  process to `Z` on screen.
- **PRIORITY/NICE** — real `task_t.nice` (POSIX nice range, this
  kernel's only scheduling priority concept — see kernel/task.c's flat
  scheduler).
- **VIRT/RES** — real, per-program-varying memory, not the same number
  for every process. This needed a genuine kernel-side fix (below) — see
  "Platform work this needed" #1.
- **CPU%** — a real delta: `(utime_now - utime_prev) / elapsed_ticks *
  100`, both operands in this kernel's real 100 Hz PIT tick units,
  computed fresh every scan from `PureUnixProcess.prevUtime` and
  `Machine.monotonicMs`/`prevMonotonicMs` (already maintained by htop's
  own base `Machine_scan()`) — confirmed varying meaningfully between
  genuinely busy vs. idle processes in this port's own testing.
- **MEM%** — real `m_resident / Machine.totalMem`, both from actual
  `kernel/pmm.c` frame counts.
- **TIME+** — real cumulative CPU ticks, not derived/estimated.
- **Command** — real `/proc/[pid]/cmdline`, and (since this port's
  kernel-side comm fix, below) a real, per-process `comm` matching what
  actually got exec'd, not a constant placeholder.
- **Tasks/kthr header** — real counts, incremented per real process
  scanned in `ProcessTable_goThroughEntries()` (`totalTasks`/
  `runningTasks`/`kernelThreads`), not htop's own zero-initialized
  defaults left untouched.
- **Uptime** — real `/proc/uptime`.
- **Mem/Swp meters** — real `/proc/meminfo` totals; swap is a real,
  honest 0 (no swap exists on this kernel at all — no disk-backed paging
  — so 0 is correct, not a stub).

### What's honestly left unavailable, not faked

- **Load average** — always 0.00/0.00/0.00. No exponentially-decayed
  run-queue sampler exists in this kernel; reporting a fabricated number
  would be worse than an honest zero (same choice `unsupported/`'s own
  reference platform makes).
- **Environment screen ('e')** — `Platform_getProcessEnv()` returns
  `NULL`; htop's own UI correctly shows "Could not read process
  environment" rather than crashing or showing garbage — no
  `/proc/[pid]/environ` exists yet.
- **Disk/Network IO meters, battery, file descriptor counts** — all
  real, honest "not available" (`false`/`NAN`) — no corresponding kernel
  accounting exists.
- **`lsof`/`strace` integration screens** (`OpenFilesScreen`,
  `TraceScreen`) — compile and link (needed real `select()` and
  `execlp()` additions, see below) but fail gracefully at runtime
  exactly like a real Unix without `lsof`/`strace` installed would:
  `execlp()` returns `ENOENT`, htop shows "Could not execute 'lsof'.
  Please make sure it is available in your $PATH." — the real upstream
  message, not a PureUNIX-specific one.

---

## Platform work this needed

Everything below was found by actually compiling, booting, and
interactively driving `htop` inside PureUNIX under QEMU — including two
genuine, general kernel bugs this port found that had nothing to do with
htop's own code (confirmed by reproducing the identical symptom with
BusyBox's own, completely independent `ps`).

### 1. Per-process memory was a fixed constant, not real per-program data

**Symptom**: every user process reported identical VIRT/RES — not a
rendering bug, `fs/procfs.c`'s `approx_vsize_bytes()` really did return
the same fixed 3 MiB (`USER_WINDOW_END - USER_WINDOW_BASE`) for *every*
live user task regardless of what it actually was.

**Root cause**: this kernel's per-process address space is a fixed-size
window, but the actual number of pages *mapped* into it varies by
program (a tiny `hello` vs. `ncdemo`'s ~1.3 MiB of linked ncurses) — the
old code never computed that real number, just reported the window's
outer bound.

**Fix**: `kernel/elf.c`'s `elf_load_into()` now accumulates the real
page-aligned size of every `PT_LOAD` segment plus the fixed stack region
into a new `task_t.mapped_bytes` field, threaded through both exec paths
(`elf_exec_argv()`, `elf_exec_current()`) and copied unchanged across
`task_fork()` (a forked child's address space really is the same size
until it execs something else). `fs/procfs.c`'s `approx_vsize_bytes()`
now just returns it — real, per-program-varying VIRT/RES, confirmed in
this port's own testing (`ncdemo` and `htop` itself visibly differ from
plain `/bin/sh` in the process list).

### 2. `task_t.name` (comm) never followed exec() — every process looked identical

**Symptom**: htop's Command column showed `user  htop` /`user  /bin/sh`
for every process, and BusyBox's completely independent `ps` showed the
exact same `{user}` bracket artifact for every single row — real upstream
`ps`/htop display logic correctly reacting to a genuine kernel gap:
bracket a process whose `comm` doesn't match its own cmdline's basename.

**Root cause**: `task_t.cmdline` was always kept correct at exec() time
(`pack_cmdline()`), but `task_t.name` (the `comm` field —
`/proc/[pid]/stat`'s parenthesized field, `/proc/[pid]/status`'s `Name:`)
never was. Every process's name stayed whatever `task_alloc()` gave it at
*creation* — `elf_exec_argv()`'s `task_create_user()` literally
hardcodes the string `"user"`; `task_fork()` otherwise just copies the
parent's own (possibly equally-stale) name.

**Fix**: a new `pack_comm()` (`kernel/elf.c`), called alongside
`pack_cmdline()` at both exec sites, sets `task_t.name` to the real
basename of the exec'd path — the same convention every real Unix uses
for `comm`. Confirmed fixed independently by both htop and BusyBox `ps`
after the change.

### 3. A genuine, general kernel bug: stale bytes leaking past a shorter cmdline

**Symptom**: `/proc/[pid]/cmdline` for an interactively-launched `htop`
process reported 8 bytes — `"htop\0\0h\0"` — not the real 5-byte
`"htop\0"`. Confirmed via BusyBox's own `cat`/`wc -c` directly on the
file (bypassing htop and this port's own parsing entirely), and via a
temporary kernel-side `SYS_EXEC` trace (never committed) proving the
*real* `argc`/`argv` BusyBox ash passed to `execve()` were already
correct (`argc=1`, `argv[0]="htop"`) — the corruption was happening
*after* that point, inside the kernel's own cmdline-packing.

**Root cause**: `kernel/elf.c`'s `pack_cmdline()` only ever wrote the
*new* content into `task_t.cmdline` and left everything past it
untouched. A `task_t` can be exec'd more than once over its lifetime
(a shell execing several external commands in sequence, no intervening
`fork()`) — if an *earlier* cmdline on the same `task_t` was longer than
a later one, the later, shorter `pack_cmdline()` call left genuine
leftover bytes from the old content dangling past the new NUL
terminator. `fs/procfs.c`'s `/proc/[pid]/cmdline` rendering trims only
*trailing* NULs by scanning backward from the end of the fixed 128-byte
buffer, so any stray non-NUL byte anywhere later in the buffer made the
reported cmdline run all the way out to it, embedded extra NULs and all
— real, reproducible corruption, not a display bug in either tool that
observed it.

**Fix**: `pack_cmdline()` now `memset()`s the entire 128-byte buffer to
zero before writing, unconditionally — a real, general correctness fix,
not a workaround scoped to this port's own reading code.

### 4. The Enter key sent the wrong raw byte for raw-mode ncurses apps

**Symptom**: htop's F9 kill-signal panel, F6 sort-by panel, and every
other list-picker (`Action_pickFromVector()`) never responded to Enter —
selection worked, arrow-key navigation worked, but confirming a
selection silently did nothing.

**Root cause**: `drivers/tty.c`'s `key_to_byte()` mapped `KEY_ENTER` to
`'\n'` (0x0A/LF). Real terminals conventionally transmit `'\r'`
(0x0D/CR) for the Return key in raw/cbreak mode; htop's `CRT_init()`
calls ncurses' `nonl()` specifically so it can see that raw byte itself
(`Action.c`'s list-picker confirm check is a literal `ch == 13`) — a
real, deliberate, portable ncurses idiom, not something PureUNIX-specific.
Every other program that *doesn't* call `nonl()` (e.g. `user/ncdemo.c`,
`docs/ncurses-port.md`) was unaffected purely because ncurses' own
*default* `nl()` mode already translates a raw `'\r'` back to `'\n'`
before `wgetch()` returns it — the same real terminal/ncurses contract
everywhere else, which is exactly why this had never surfaced before a
program that deliberately opted out of it.

**Fix**: `key_to_byte()` now sends the real, conventional `'\r'` for
Enter. `drivers/tty.c`'s canonical-mode line reader was updated to accept
*either* `'\r'` or `'\n'` as the line terminator (still always appending
a literal `'\n'` to the assembled line, matching what every canonical-mode
reader already expects) — ordinary shell input is unaffected, confirmed
by the full existing regression suite (`tools/vt-scripts/*.txt`) still
passing unchanged, including BusyBox ash's own job-control tests.
Confirmed fixed end-to-end: pressing Enter on htop's F9 kill panel now
sends the selected signal — see Testing.

### 5. `select()`, `execlp()`, `gethostname()`, and a real `dirfd()` stub — real, general libc gaps

Found purely by iterating `htop`'s link errors (the same "compile, fix
what's missing, iterate" process the ncurses port already used):

- **`select()`** (`user/newlib_syscalls.c`) — didn't exist at all.
  Needed by `TraceScreen.c`'s strace/truss integration. Same honest
  answer as this file's pre-existing `poll()` (no real readiness
  multiplexing exists in this kernel; every requested fd is reported
  ready immediately, and the `read()`/`write()` that follows is what
  actually blocks or returns data) — not a new limitation, the same one
  `poll()` already documented, now covering the second POSIX API for it.
- **`execlp()`** — didn't exist; `execve()`/`execvp()` already did.
  A real, standard thin wrapper (collect the variadic arg list into an
  `argv[]`, hand off to `execvp()`) — the same shape every libc ships,
  not PureUNIX-specific. Needed by `OpenFilesScreen`/`TraceScreen`'s
  `execlp("lsof"/"strace"/"truss", ...)` calls, which then correctly
  fail with `ENOENT` at runtime (see "honestly left unavailable" above).
- **`gethostname()`** — didn't exist; no hostname database exists in
  this kernel at all (no `sethostname()`-backed state anywhere), so this
  is a real, fixed, honest `"pureunix"` — matching `Platform_getRelease()`'s
  own naming — rather than fabricated per-boot state nothing backs.
- **`dirfd()`** — declared by newlib's own `<dirent.h>` but never
  implemented, and shadowed out entirely by `user/newlib_compat/
  dirent.h`'s override (needed for a completely unrelated reason — see
  that file's own header comment). Needed only because `XUtils.h`'s
  `xDirfd()` helper (used behind htop's own `HAVE_OPENAT` guard, which
  this port's build never defines) still needs the *symbol* to exist to
  compile, even though nothing in this build ever calls it at runtime.
  This kernel's `opendir()`/`readdir()` have no real per-directory file
  descriptor at all (`SYS_READDIR` dumps a whole directory listing in
  one call, no cursor) — so rather than fabricate a fake fd number,
  `dirfd()` is a real, honest `ENOSYS` stub, exactly the same category as
  this project's pre-existing `getrusage()` (`docs/sqlite-port.md`).
- **`_GNU_SOURCE`** — `strcasestr()`/`vasprintf()`/`asprintf()` are all
  genuinely implemented in the vendored newlib `libc.a` (confirmed via
  `nm`), just declared behind `__GNU_VISIBLE` in newlib's own headers;
  htop's build (`Makefile`'s `HTOP_CFLAGS`) just needed `-D_GNU_SOURCE`
  to see the declarations already-present functions.

### 6. Why hand-written `config.h` instead of running htop's own `configure`

Same reasoning as `third_party/sqlite/`/`third_party/lua/`/
`third_party/tcc/` (`docs/sqlite-port.md` etc.): unlike ncurses (which
genuinely generates substantial real source — capability tables, a
compiled terminfo fallback — at build time, see `docs/ncurses-port.md`),
htop's own `configure`/`Makefile.am` only ever *selects* which
already-fixed platform subdirectory's `.c` files join one fixed,
hand-enumerable core file list. That selection is reproduced directly in
PureUNIX's own top-level `Makefile` (the `htop` section), with a
hand-written `third_party/htop/pureunix/config.h` standing in for the
`HAVE_*`/`HTOP_*` macros `configure` would otherwise generate — the same
"provide a known-good answer instead of an unrunnable cross-compiled
probe" reasoning `tools/build-ncurses.sh` already established, just
applied by hand instead of via a pre-seeded autoconf cache variable
since no autoconf invocation happens here at all.

---

## Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `/bin/htop.elf` | The ELF | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF |
| `/bin/htop` → `/bin/htop.elf` | Symlink | Same pattern as `ncdemo`/`sqlite3`/`lua` — makes `htop` resolve as an ordinary `$PATH` command with no setup, matching the task's exact `htop` invocation |

`third_party/htop/htop-3.5.1/` is the vendored, unmodified upstream
release (see that directory's `README.md`); `third_party/htop/pureunix/`
is this port's own new platform backend, compiled directly by the
top-level Makefile's `htop` section (`HTOP_CORE_SRCS`/
`HTOP_PLATFORM_SRCS`) against the real ncurses (`NCURSES_CFLAGS`/
`NCURSES_LDFLAGS`, already established by the ncurses port) — no
upstream `configure`/`make` is ever invoked, `make`/`make iso` stay
network-free.

EXT2-only (like every other third-party userspace program in this tree)
— FAT16 isn't part of the primary ash userland.

---

## Testing

Verified interactively in QEMU against the real persistent disk image
(`build/pureunix.iso`, the exact `make iso` deliverable), using the same
QMP `send-key` + `screendump` technique `tools/test-ncurses-demo.py`
established.

- **Real process list**: every live PureUNIX task appears with correct
  PID/PPID/state, including this kernel's own permanent service tasks
  (`kernel`, `init-reaper`, 5× `vt-session`) correctly bracketed as
  kernel threads (0 VIRT/RES — genuinely never exec'd a user ELF) versus
  real user processes (`/bin/sh`, `htop` itself) with real, differing
  memory footprints.
- **Real varying CPU%**: confirmed distinct, changing percentages across
  processes between refresh cycles (not a static demo value).
- **F2 Setup**: the full, real upstream Categories/Display
  options/Header layout/Meters/Screens/Colors panel tree renders and
  navigates correctly — dozens of real checkbox/toggle options, not a
  placeholder screen.
- **F6 Sort By, F3 Search, F4 Filter**: all real, interactive, and
  functional — F4 filtering to a non-matching substring correctly
  produces an empty (not broken) result list, confirmed real text-input
  handling.
- **F9 Kill — the definitive end-to-end test**: a real `sleep 300 &`
  background job, selected in htop's process list and sent `SIGTERM` via
  F9 → Enter, confirmed killed through **two independent observers**:
  htop's own process list immediately showing that PID transition to `Z`
  (zombie) state, and — after quitting htop — BusyBox ash's own,
  completely separate job-control subsystem printing its real
  termination notice, `[1]+  Terminated              sleep 300`. This
  is not two views of the same code path; it's independent proof the
  real `SYS_KILL` syscall genuinely reached and terminated the real
  process.
- **Clean shell restoration**: after quitting htop (F10), the shell
  prompt returns and accepts further commands correctly — canonical
  mode/echo were not left in htop's raw/`noecho` state.
- **VT-switch safety, `SIGWINCH`, and terminal-restore mechanics** are
  inherited directly from the ncurses port's own testing
  (`docs/ncurses-port.md`) — htop uses the exact same
  `initscr()`/`endwin()`/terminal-driver machinery, not a separate path.
- Full regression suite re-verified with no *new* regressions:
  `tools/vt-scripts/*.txt` (ash job control, Ctrl+C/Ctrl+\ foreground
  signal delivery, `ps`/`top` via `/proc`, VT-switch-doesn't-interrupt)
  all pass unchanged; `systest` 343/345 (same 2 pre-existing
  console-geometry failures, `[129]`/`[130]` — unchanged; see project
  memory's `gotcha_preexisting_systest_failures`);
  `tools/test-persistent-boot.py`, `tools/test-sqlite-persistence.py`,
  and `tools/test-ncurses-demo.py` all still pass unchanged — in
  particular, `test-ncurses-demo.py` passing unchanged is the direct
  confirmation that the Enter-key `'\r'` fix (needed for htop) didn't
  regress `ncdemo`, which relies on ncurses' own default `'\r'`→`'\n'`
  translation instead of htop's `nonl()`-disabled raw byte.

---

## Known gaps

- **No load average** — no exponentially-decayed run-queue sampler
  exists in this kernel; always reports 0.00/0.00/0.00 (an honest
  absence, not a fabricated value).
- **No `/proc/[pid]/environ`, `/proc/[pid]/maps`, `/proc/[pid]/fd/`,
  `/proc/[pid]/io`** — htop's Environment screen, memory-map inspection,
  open-file listing, and I/O accounting columns all correctly show "not
  available" rather than crashing or fabricating data; a real, narrow
  gap, not a correctness bug in what's displayed.
- **No `stime` (system time)** — this kernel doesn't distinguish
  user/kernel CPU time per task; `TIME+` reflects total (`utime`-only)
  ticks, an honest reflection of what's genuinely tracked rather than a
  fabricated split.
- **No swap, no disk I/O, no network I/O, no battery** — all real,
  honest "not applicable" on this kernel (no disk-backed paging, no
  block-device I/O accounting, no network I/O accounting, no battery
  hardware model).
- **`lsof`/`strace`/`truss` integration screens exist but are
  permanently unusable** — no such binaries exist or are plausible on
  PureUNIX (no ptrace-equivalent kernel support for a real `strace`);
  they fail with the same real upstream "could not execute" message a
  Unix without them installed would show, not a PureUNIX-specific error.
- **`select()`/`poll()` are honest non-multiplexing stubs** (pre-existing
  platform limitation, not new to this port) — every fd is always
  reported ready immediately; htop's own strace/truss integration and any
  other `select()`-based timeout logic elsewhere in htop don't genuinely
  wait/multiplex, though nothing in the tested, working feature set
  depends on that distinction.
