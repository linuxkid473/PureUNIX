# Qt 6 port — blocker map (Phase 1) + real C++ runtime (Phase 2, done)

Status: Phase 1 (audit) and Phase 2 (real C++ userspace foundation) are
done and QEMU-verified. Phase 3 (cross-compiling actual Qt 6) starts next.
This document is the living record of the Qt 6 port (pinned version,
toolchain, patches, protocol, architecture, limitations) required by the
port's own acceptance criteria.

Pinned target: **Qt 6.5.3** (last Qt 6 LTS-track minor with a plain qtbase
tarball release and no hard CMake-version creep beyond what's reasonably
buildable against a from-scratch platform; revisit only if qtbase 6.5.3's
CMake scripts turn out to assume something newer). Modules: **qtbase**
only — Core, Gui, Widgets. Everything else (Quick/QML/WebEngine/Network/
SQL/DBus/Bluetooth/OpenGL/Vulkan/accessibility/printing/platformthemes/
plugins) deliberately excluded per the task brief.

## 1. Toolchain (Phase 2 blockers)

- Host has `i686-elf-gcc`/`i686-elf-g++` 16.1.0 (Homebrew), configured
  `--without-headers --enable-languages=c,c++`. **g++ exists but
  libstdc++ was never built** (bootstrap-only GCC, no target libc existed
  at GCC-build time). No `libstdc++.a`, no `include/c++/16.1.0/` anywhere
  on the host. This is the #1 blocker for Phase 2.
- The repo's root `Makefile` has zero C++ wiring: no `CXX`, no C++ flags,
  no C++ file ever compiled by it. Every existing "port" (SQLite, Lua,
  ncurses, SDL2, zlib, libpng, chocolate-doom, htop, newlib) is C only;
  73 stray `.cpp`/`.cc` files exist in the repo but all are *unbuilt*
  upstream files inside vendored trees (Lua's optional C++ header, SDL2's
  non-PureUnix platform backends, ncurses' optional C++ bindings, etc).
- No CMake is invoked anywhere in this repo today. Every third-party port
  either hand-lists upstream `.c` files in the top-level Makefile
  ("Pattern A"), or vendors the *prebuilt output* of a real upstream
  `configure`/`make` run done once, offline, via a `tools/build-*.sh`
  script (newlib, ncurses, busybox — "Pattern B"). Qt6's own build
  *requires* CMake (qmake is no longer how Qt itself is built in Qt6) —
  this will be the first CMake cross-compile this repo has ever done.
- Fix plan: cross-build `libstdc++-v3` from matching GCC 16.1.0 source
  against the already-vendored newlib, using the Pattern-B precedent
  (`tools/build-newlib.sh`) — write `tools/build-libstdcxx.sh`, vendor the
  result at `third_party/libstdcxx/i686-elf/{include,lib}`.

## 2. C++ runtime semantics (Phase 2 blockers)

- **No `.init_array`/`.fini_array` support anywhere.** Confirmed by
  reading both crt0 variants (`user/crt0.S`, `user/newlib_crt0.S` +
  `user/newlib_crt0.c`) and `kernel/elf.c`: the ELF loader only walks
  `PT_LOAD` program headers and never touches the section-header table at
  all. **Global C++ constructors currently cannot run for any program on
  this OS.** Good news: this needs zero kernel change. `.init_array` /
  `.fini_array` live inside a normal `PT_LOAD`-covered segment (already
  eagerly copied faithfully by the existing loader); the fix is entirely
  userspace — add the sections + `__init_array_start/end` symbols to
  `user/linker.ld`, and walk them from a new C++-aware crt0 before
  `main()` (and drive `.fini_array` from `atexit()`/newlib `exit()`).
- guard variables (`__cxa_guard_acquire/release`), `__cxa_pure_virtual`,
  `operator new/delete` — all supplied by libsupc++ (part of the
  libstdc++-v3 source tree), not something to hand-write.
- RTTI: PureUnix has **no dynamic linking at all** (ELF loader only
  accepts `ET_EXEC`, never `ET_DYN`; no `PT_DYNAMIC`/`PT_INTERP`; no
  `dlopen`/`dlsym` anywhere). Every program is a single flat statically
  linked binary. This is actually a simplifying fact for RTTI/typeinfo:
  the classic cross-shared-object typeinfo-identity problem that RTTI
  needs `-frtti`'s string-compare fallback for doesn't arise here — one
  binary, one set of vague-linkage-merged typeinfo objects, plain pointer
  identity works. No OS support needed for RTTI.
- Exceptions: DWARF CFI unwinding only touches the binary's own
  `.eh_frame`/libgcc unwinder, no OS syscalls involved. Should work
  freestanding. Real risk is Qt6 build-time: confirm qtbase 6.5.3 builds
  cleanly with exceptions+RTTI enabled for this target before deciding
  whether `-fno-exceptions`/`-fno-rtti` is actually necessary (task
  permits disabling them if upstream Qt supports the resulting config —
  don't disable preemptively).
- Atomics: i686 `lock cmpxchg` works as a plain machine instruction
  regardless of whether the OS has real threads — no OS support needed.
  Qt's extensive `QAtomicInt`/implicit-sharing refcounting will work as
  compiled code with zero additional OS plumbing.
- TLS (`.tdata`/`.tbss`): **not implemented** by the ELF loader (no
  `PT_TLS` handling) or crt0. Newlib itself was built
  `--disable-newlib-multithread`, so newlib's own `errno`/reentrancy
  struct is a single global already (no TLS needed there). Real
  `thread_local` C++ storage duration is out of scope — acceptable, since
  Qt Core/Gui/Widgets can be built single-threaded (see below) and this
  target has no threads to begin with.

## 3. Threading (Phase 2/3 blocker — new userland shim needed)

- **No threading of any kind exists** — no `pthread_create`, no
  `clone()`, no kernel thread concept; `task_fork()` always deep-copies
  the entire address space (no `CLONE_VM`). No mutex/semaphore/condvar
  primitive at any layer (kernel-internal `wait_queue_t` is not exposed
  to userland). Confirmed by exhaustive grep — genuinely absent, not just
  undocumented.
- Qt's Unix backend implements `QMutex`/`QThread`/`QThreadStorage` as
  thin wrappers over real `pthread_*` — even a single-threaded Qt
  application still compiles/links against `pthread_mutex_t` etc. (e.g.
  signal/slot connection-list locking).
- Fix plan: a **minimal single-threaded pthread-compatibility shim**
  (new userland library, not a kernel feature) — `pthread_mutex_t`
  lock/unlock as no-ops (there is provably only ever one thread), a small
  static-array-backed `pthread_key_create`/`get/setspecific`, `pthread_once`
  doing the obvious thing, `pthread_self()` returning a constant,
  `pthread_create()` deliberately returning an error (mirrors the TCC
  port's precedent of deliberately forcing `static_link=1` under
  `TCC_PUREUNIX` for a known-absent OS facility, rather than
  half-implementing it). Qt must be configured `-no-feature-thread` /
  equivalent so `QThread::start()` is never actually exercised.

## 4. Qt Core event dispatcher (Phase 3/4 blocker — smallest real new syscall)

- No `select()`/`poll()`/`epoll` exist at the kernel level; the
  newlib-glue `poll()`/`select()` are fake stubs that report every fd
  immediately ready (fine for existing programs, which only follow up
  with a real blocking read/write). Qt's event dispatcher needs to
  **block on an fd with a timeout** (wait for the external GUI protocol
  pipe, but wake up early for a due `QTimer`) — the one piece of
  currently-missing general OS machinery this port actually needs at the
  kernel level.
  - Fix plan: add a real, minimal `SYS_POLL`-equivalent (or a
    timeout-capable blocking pipe-read mode) to the kernel — the smallest
    correct general primitive, not a Qt-specific hack, so it's usable by
    any future ported program that needs readiness-with-timeout.
- `clock_gettime(CLOCK_MONOTONIC)` doesn't exist, but `SYS_GET_TICKS_MS`
  (syscall 59, PIT-derived, 10ms resolution, already general-purpose)
  provides exactly the needed semantics — just needs a newlib glue
  wrapper, no kernel change.

## 5. PUDE / external GUI client protocol (Phase 5 — smaller than feared)

- PUDE (`pude.elf`) is one monolithic process; all its "apps" (PUTerm,
  Calculator, PUFiles, PUText) are compiled-in `app_class_t` vtables
  called as plain in-process C function pointers — **zero IPC** in
  PUDE's existing app model.
- The compositor does a full-frame redraw every changed iteration (no
  damage-rect concept at all today); `SYS_FB_BLIT` is a hardcoded
  whole-framebuffer operation.
- SDL2 apps (including `pude.elf` itself) get **exclusive, direct**
  framebuffer access via `SYS_FB_MMAP`/`SYS_FB_BLIT`, entirely bypassing
  any compositor. The closest existing precedent for launching another
  graphical program (PUFiles → `imgview.elf`) is a **blocking full-screen
  handoff**: fork/exec, then the launcher blocks in `waitpid()` while the
  child owns the entire screen exclusively — not real windowing, no
  compositing of two processes' pixels together, ever.
- **No shared-memory syscall exists anywhere.** No syscall accepts a
  target-process id or window/surface handle. `SYS_INPUT_POLL` is
  per-VT, not per-window.
- Good news: this does **not** require a new shared-memory syscall.
  Pipes are already a fully-worked primitive (used exactly this way by
  PUTerm's pty-connected shell child). The plan: spawn the external GUI
  client with two pipes (client→PUDE: window-create/title/damage-rect+
  pixel-bytes/close-request; PUDE→client: input events/resize/close),
  symmetric to how PUTerm already wires up a pty pair to its shell child.
  PUDE-side, the external client becomes just another `app_class_t`-shaped
  adapter in its existing window list/z-order/focus model — no changes
  to PUDE's core WM loop. No new kernel syscalls needed for this phase
  beyond the Phase 4 poll-with-timeout addition (the client's own event
  loop needs it too, to service both the input pipe and Qt timers).

## Net new OS/build facilities this port requires

1. `tools/build-libstdcxx.sh` — cross-build libstdc++-v3 against vendored
   newlib (build tooling; Pattern-B precedent). **Done.**
2. `.init_array`/`.fini_array`/`.ctors`/`.dtors` walking in a new
   C++-aware crt0 + linker script sections (userspace only, no kernel
   change). **Done.**
3. Minimal single-threaded `pthread` compatibility shim (new userland
   library, deliberately stubbed per the TCC-port precedent). Not started
   yet — needed once Qt Core/Gui actually get built (Phase 3/4).
4. One real new kernel syscall: blocking-read/poll with a timeout
   (general-purpose, not Qt-specific). Not started yet (Phase 4).
5. `clock_gettime(CLOCK_MONOTONIC)` newlib glue wrapper over the existing
   `SYS_GET_TICKS_MS`. Not started yet (Phase 4).
6. External PUDE GUI client protocol over two pipes (no new syscalls;
   reuses `SYS_PIPE`/`SYS_FORK`/`SYS_EXEC`), plus a PUDE-side adapter
   `app_class_t` to host it in the existing window/compositor model. Not
   started yet (Phase 5).

## Phase 2 results: a real C++ runtime on PureUnix (done, QEMU-verified)

`third_party/libstdcxx/i686-elf/` now vendors a real cross-built
libstdc++-v3 + libsupc++ (GCC 16.1.0, matching the installed i686-elf-g++
exactly) via `tools/build-libstdcxx.sh`, following the same "vendor a
prebuilt build of real upstream source" pattern as `tools/build-newlib.sh`.
`user/cxxtest.cpp` (35 checks, `tools/vt-scripts/run-cxxtest.txt`) proves
all of the following work for real, on-target, in QEMU — not just at
compile time:

- global constructors run before `main()`, in declaration order
- global destructors run at `exit()`, in reverse order (independently
  verified by a destructor that prints the observed order itself)
- local static initialization (guard variables) runs exactly once
- `new`/`delete` and `new[]`/`delete[]`
- virtual dispatch and pure virtual functions through a real vtable
- `std::string` (including past the SSO threshold — genuine heap use),
  `std::vector`, `std::unique_ptr`, `std::sort`
- mixed C/C++ libc interaction (`strlen`/`strcmp` on `std::string::c_str()`)
- **real C++ exceptions** — a genuine `throw`/`catch` through a normal
  call frame, DWARF CFI unwinding, `what()` — not disabled

Four real bugs were found and fixed to get there, none of them
Qt-specific — all are general C++-on-PureUnix runtime facts:

1. **libstdc++-v3's own configure couldn't link a test program at all.**
   i686-elf-gcc has no start files (`crt0.o`/`crti.o`/`crtn.o` — it was
   built `--without-headers`), so its very first "does this compiler
   work" link probe failed, which autoconf's `GCC_NO_EXECUTABLES` macro
   then latches into a permanent "no more link tests allowed" mode that
   hard-errors on later `AC_CHECK_FUNCS` calls it has no graceful
   fallback for. Fixed with `-nostartfiles` plus linking in the same
   `newlib_crt0_asm.o`/`newlib_crt0.o`/`newlib_syscalls.o` every real
   PureUnix newlib program already uses (newlib's own libc.a calls bare
   `close`/`read`/`write`/etc. per its `-DMISSING_SYSCALL_NAMES` build —
   see `tools/build-newlib.sh` — so even a trivial `int main(){}` needs
   them to link). This LDFLAGS override only applies at `./configure`
   time; the real `make`/`make install` invocations override it back down
   to plain `-nostartfiles -L...` on the command line, since libtool's
   convenience-archive step rejects raw `.o` files passed via LDFLAGS.
2. **`-ffreestanding` silently produces a crippled libstdc++.** It makes
   GCC predefine `__STDC_HOSTED__=0`, which libstdc++ headers read to
   decide whether to expose the real ("hosted") library at all —
   `<bits/requires_hosted.h>` `#error`s out, `getenv`/`__throw_bad_alloc`/
   iostream all disappear. PureUnix genuinely is hosted here (real
   newlib libc, real syscalls) — `-ffreestanding` is deliberately *not*
   used for the libstdc++ build or for any `.cpp` file compiled against
   it (`USER_CXXFLAGS` in the Makefile), unlike the rest of PureUnix's
   (C) userland.
3. **`user/newlib_compat/stdio.h`'s `#define getline __getline` broke
   `std::getline`.** That shim exists for a real reason (newlib only
   exposes `__getline()`, never the POSIX name plain C code expects), but
   applied blindly it also renames libstdc++'s own unrelated internal
   `std::getline` machinery mid-header, at different points in different
   headers depending on include order, producing "template-id
   `__getline<>` ... does not match any template declaration". Fixed by
   scoping the macro to `#ifndef __cplusplus` — no C++ code needs the
   C-library alias, C++ callers use `std::getline` directly.
4. **Real exceptions took an invalid-opcode fault at the first `throw`.**
   libgcc's DWARF unwinder here uses the classic `__register_frame()`
   registered-linked-list lookup (confirmed by the total absence of any
   `dl_iterate_phdr` reference in libgcc.a — that's the alternative
   mechanism a real dynamic loader would provide, which PureUnix has
   none of). Nothing was ever registering this binary's `.eh_frame`
   contents with the unwinder — normally `crtbegin.o`'s own constructor
   does this, and PureUnix has no `crtbegin.o`. Fixed by giving
   `user/linker.ld`'s `.eh_frame` output section explicit
   `__eh_frame_start`/`__eh_frame_end` symbols and calling
   `__register_frame()`/`__deregister_frame()` (real, exported-but-
   undocumented libgcc entry points — no header exists for them, same as
   other bare-metal ports) at process start/exit in
   `user/newlib_crt0.c`.

One more real, non-obvious fact (not a bug, just a surprise worth
recording): **this GCC 16.1.0 build emits legacy `.ctors`/`.dtors`
sections for ordinary global constructors, not the modern
`.init_array`/`.fini_array`.** `-fuse-init-array` doesn't exist on this
GCC version (it was a config-time-only decision even when it did exist on
older GCCs) — verified directly by objdumping a compiled `.o` and finding
an empty `.init_array` but a populated `.ctors`. `user/linker.ld` and
`user/newlib_crt0.c` walk *both* conventions (whichever is actually
populated does the real work); order between the two doesn't matter
since cross-translation-unit constructor order is unspecified by the C++
standard anyway (only within-TU order is guaranteed, and the compiler
already gets that right by bundling a TU's global constructors into one
`_GLOBAL__sub_I_*` wrapper). Destructors for ordinary global objects turn
out to always go through `__cxa_atexit` (registered by the constructor
itself at construction time) rather than a populated `.dtors`/
`.fini_array` entry in practice — the fini-array/dtors walk is defensive/
empty in the common case, not dead code.

None of the above required a single kernel change — everything so far is
userspace build tooling (`tools/build-libstdcxx.sh`), the linker script
(`user/linker.ld`), and crt0 (`user/newlib_crt0.c`). Confirmed no
regressions: the full existing build (`make`) relinks every newlib-linked
program (BusyBox, TCC, Lua, SQLite, ncurses, SDL2, PUDE, Chocolate Doom,
htop, imgview, ziptest — all share the same crt0/linker.ld) cleanly, and
`user/libctest.c`/`dirtest.c`/`exectest.c` (45/14/16 checks) plus the
`user/systest.c` baseline (343/345, the 2 pre-existing known
console-geometry failures — see project memory) all still pass unchanged.

## Known major risk not mitigated by the above

Qt6's CMake configure runs extensive `try_compile`/`try_run`
feature-detection against the target toolchain; cross-compiling means
`try_run` cannot execute target binaries on the host at all (the same
class of problem already hit and solved once in this repo for ncurses'
`poll()` probe — solved there by pre-seeding a cache variable). Qt6's
CMake feature system is far larger than ncurses' single autoconf probe;
expect many iterations of "a try_compile/try_run fails, force the
answer via an initial CMake cache file, re-run configure" before qtbase's
configure completes cleanly. This is the single biggest time sink in the
whole port and has no shortcut — it is systematically ground through, not
solved in one step.
