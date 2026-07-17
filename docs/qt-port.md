# Qt 6 port — blocker map, C++ runtime, and real Qt 6.5.3 cross-build

Status: Phase 1 (audit), Phase 2 (real C++ userspace foundation), Phase 3
(cross-compiling real Qt 6.5.3 qtbase — Core+Gui+Widgets) and **Phase 4
(native Qt Core test on PureUnix, in QEMU) are all done.**
`third_party/qt/i686-elf/lib/libQt6{Core,Gui,Widgets}.a` are real,
verified i686 ELF static libraries containing genuine Qt6 symbols (not a
compile-only partial result — the full qtbase build completed and linked
cleanly), and `user/qtcoretest.cpp` — real `QCoreApplication`, `QString`,
`QByteArray`, `QList`, `QFile`, `QElapsedTimer`, a real moc-generated
signal/slot connection, and a real running `QCoreApplication::exec()`
event loop driven by three staggered `QTimer::singleShot()` callbacks —
**passes all 20/20 checks running natively on real PureUnix in QEMU**
(`tools/vt-scripts/run-qtcoretest.txt`, part of `make run-test`'s
regression suite). **Milestone reached: QtCore runs natively on
PureUnix.** Phase 4 needed two real, general kernel additions beyond what
Phase 1's audit anticipated (real i386 TLS support, and a real `poll()`
that can actually check pipe readiness) — see "Phase 4 results" below.
Phases 5-8 (QtGui, the PUDE external-client protocol, QtWidgets, the final
demo) not started. This document is the living record of the Qt 6 port
(pinned version, toolchain, patches, protocol, architecture, limitations)
required by the port's own acceptance criteria.

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
3. Minimal single-threaded `pthread` compatibility shim. **Turned out
   unnecessary**: `libQt6Core.a` was built with `FEATURE_thread=OFF` and
   has zero undefined `pthread_*` symbols anywhere (confirmed by `nm`) —
   Qt's own single-threaded configuration avoids the pthread dependency
   entirely rather than needing a stub for it. Revisit only if a future
   phase (Gui/Widgets) turns out to pull one in.
4. A real `poll()` that can genuinely check readiness instead of always
   lying "ready" — **done (Phase 4)**, see "Phase 4 results" below
   (`SYS_POLL`, `arch/i386/syscall.c`).
5. `clock_gettime(CLOCK_MONOTONIC)` newlib glue wrapper over the existing
   `SYS_GET_TICKS_MS`. **Done** — landed in Phase 3 (see item 11 above),
   this list just hadn't been updated yet.
6. Real i386 TLS (thread-local storage) support — **done (Phase 4)**, see
   "Phase 4 results" below (`SYS_SET_TLS`, a new GDT descriptor, and
   `user/newlib_crt0.c`'s `tls_init()`). Not anticipated by Phase 1's
   audit at all (that audit only flagged TLS as "out of scope, no current
   userspace need" — Qt6's `QBindingStorage` turned out to be the first
   real need).
7. External PUDE GUI client protocol over two pipes (no new syscalls;
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

## Phase 3 results: real Qt 6.5.3 cross-compiled for PureUnix (done)

`third_party/qt/i686-elf/lib/libQt6Core.a` (12 MB), `libQt6Gui.a` (13 MB),
`libQt6Widgets.a` (15 MB) are real, cross-compiled i686 ELF static
archives — verified by `file` (`ELF 32-bit LSB relocatable, Intel 80386`)
and `nm` (real mangled Qt symbols, e.g. `qHash(QStringView, unsigned
long)`) on individual extracted `.o` members. `libQt6Xml.a` and the
bundled dependency libraries (zlib, libpng, libjpeg, FreeType, HarfBuzz,
PCRE2) came along as transitive requirements of Gui/Core. `qminimal`/
`qoffscreen` QPA plugins and `qgif`/`qico`/`qjpeg` image-format plugins
were built too (static, meant to be explicitly linked in — no dynamic
plugin loading exists on PureUnix, see section 2).

The predicted "biggest risk" (extensive CMake `try_compile`/`try_run`
feature detection, cross-compiling meaning `try_run` can't execute target
binaries) turned out smaller than feared: setting
`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` in `tools/pureunix-qt-toolchain.cmake`
— a real, sanctioned CMake mechanism for exactly this "compiler can't link
a runnable executable without extra board-specific startup collateral"
scenario — makes essentially all of Qt's own `try_compile`-based feature
checks succeed as ordinary object-file compiles, sidestepping the need to
hand-answer a huge cache-variable grind. Only **one** real CMake config
test (`config.tests/arch` — it inspects a linked binary's bytes for
embedded architecture-marker strings, so it genuinely needs a real
executable) needed the full `-nostartfiles` + real `newlib_crt0`/
`newlib_syscalls` objects treatment already established in
`tools/build-libstdcxx.sh`. No cache-variable pre-seeding grind was needed
at all.

The real work turned out to be a sequence of specific, one-at-a-time
build errors — each fixed once, globally, via `tools/pureunix-qt-toolchain.cmake`
flags or a real minimal upstream patch (`third_party/qt/patches/`), not a
generic try_compile/try_run guessing game:

1. **Qt has never heard of PureUnix.** `src/corelib/global/
   qsystemdetection.h` `#error`s out for any OS it doesn't recognize.
   Fixed with a genuine, minimal upstream patch
   (`0001-add-Q_OS_PUREUNIX.patch`) adding a `Q_OS_PUREUNIX` branch,
   mirroring the existing `Q_OS_VXWORKS` case (set from the mkspec via a
   `#define`, not autodetected — PureUnix has no predefined compiler
   macro of its own, same as VxWorks). `Q_OS_UNIX` falls out automatically
   from the file's own "not Windows -> Unix" fallback.
2. **`int32_t`/`uint32_t` is `long`/`unsigned long` on this newlib target,
   not plain `int`/`unsigned int`** (both are 4 bytes on i686 — a pure
   strict-typedef mismatch, not a real bug, but GCC 14+ escalated
   `-Wincompatible-pointer-types` from a warning to a hard C-language
   error by default). Hits bundled 3rdparty C sources written assuming
   glibc's convention (e.g. PCRE2) and, in C++, bundled HarfBuzz code
   passing an `hb_codepoint_t*` where Qt's own `glyph_t*` is expected.
   Fixed with `-Wno-incompatible-pointer-types` (C) and `-fpermissive`
   (C++ — GCC's own error message names this flag directly) globally.
3. **Bundled PCRE2's JIT backend (sljit) hard-requires `pthread_mutex_t`**
   and generates+executes code at runtime — no threading exists on
   PureUnix (section 3), and there's no real W^X/self-modifying-code
   story on this target worth building for regex JIT specifically. Fixed
   with `-DPCRE2_DISABLE_JIT=1` (QNX/UIKIT already get this exact define
   for their own unrelated reasons in `src/3rdparty/pcre2/CMakeLists.txt`
   — passed globally via a compiler define instead of editing that file).
4. **Bundled HarfBuzz's mutex falls back to `std::mutex`**, which needs
   real gthreads support libstdc++ doesn't have here (single-threaded
   `gthr-single` backend, confirmed during Phase 2). HarfBuzz has its own
   built-in no-op single-threaded fallback for exactly this case
   (`hb-mutex.hh`'s `#else` branch), gated on one define: `-DHB_NO_MT=1`.
5. **Qt's `qfloat16` uses GCC's native `_Float16` type**, which requires
   `-msse2` to actually use arithmetically — `__FLT16_MAX__` (the macro
   gating this) is predefined by x86 GCC regardless of `-march`, so this
   isn't caught by our `-march=i686` choice. **This repo's kernel
   context-switch code (`kernel/task.c`, `arch/i386/*`) does not save or
   restore FPU/SSE state at all** (confirmed: zero `fxsave`/`fxrstor`/
   `xsave` anywhere, not even for x87) — a real, separate, unfixed
   architectural gap well beyond this port's scope. Enabling SSE2 codegen
   for any userspace code before that's fixed would silently corrupt XMM
   register state across a context switch landing mid-computation. Fixed
   by undefining the one builtin macro (`-U__FLT16_MAX__`), forcing
   `qfloat16` into its portable plain-`float`-based fallback path instead
   — sidesteps the SSE2 requirement entirely without touching the kernel.
   **Any future PureUnix work involving real floating-point-heavy
   userspace code (not just Qt) should treat FPU/SSE context-switch
   support as a real prerequisite, not assume it already exists.**
6. **CMake's own `UNIX` variable was false**, because `CMAKE_SYSTEM_NAME
   Generic` (tried first, the natural "bare-metal/no-OS" choice) signals
   exactly that — no OS at all — to CMake, which excludes qtbase's real
   POSIX source files (`qfilesystemengine_unix.cpp`,
   `qeventdispatcher_unix.cpp`, `qcore_unix.cpp`, `qthread_unix.cpp`, ...,
   all gated `CONDITION UNIX` in `src/corelib/CMakeLists.txt`) from the
   build entirely, while *other* Qt source still `#include`s their
   headers unconditionally based on the C++-level `Q_OS_UNIX` macro
   (which the patch above does define) — a mismatch between the
   CMake-level and preprocessor-level "is this Unix" signals. PureUnix
   genuinely provides the POSIX shape those files assume (real
   `fork`/`open`/`read`/`write`), so `CMAKE_SYSTEM_NAME Linux` is used as
   a pragmatic proxy purely to make CMake's own `UNIX` variable true —
   not a claim of glibc/Linux-syscall compatibility (nothing at the C++
   preprocessor level ever thinks it's Linux; only `Q_OS_PUREUNIX`/
   `Q_OS_UNIX` are ever defined).
7. **`sigaction()` the function (not the struct) and `getentropy()` were
   invisible** despite newlib providing real implementations, because
   newlib's `sys/features.h` only exposes BSD/POSIX-2008 declarations
   when nothing more restrictive got defined first, and some libstdc++
   header ends up narrowing `_POSIX_C_SOURCE`/`_XOPEN_SOURCE` before Qt's
   C++ sources reach `<signal.h>`/`<unistd.h>` (plain C builds like
   `libctest.c` never hit this). Fixed with `-D_GNU_SOURCE` globally —
   the strongest, first-checked knob in that file, forcing maximal
   visibility regardless of what else got defined first.
8. **`qlibrary_unix.cpp` needs `<dlfcn.h>`**, which doesn't exist (no
   dynamic loading on PureUnix at all). `QT_FEATURE_library` is itself
   conditioned on `QT_FEATURE_dlopen` in qtbase's own `configure.cmake`,
   so disabling dlopen (`-DFEATURE_dlopen=OFF`) correctly excludes it —
   no patch needed, just the right feature flag (`-DINPUT_opengl=no`
   rather than `-DFEATURE_opengl=OFF` was needed earlier for the same
   "raw INPUT var vs FEATURE var" reason, discovered when a hard
   CMake-level `qt_configure_add_report_entry(TYPE ERROR ...)` fired
   despite `FEATURE_opengl=OFF` being set).
9. **`getgrgid()` invisible in `qfilesystemengine_unix.cpp`** despite
   `<pwd.h>` being included — a genuine upstream Qt oversight (some libcs'
   `<pwd.h>` transitively pulls in `<grp.h>`; newlib keeps them fully
   separate). Fixed with a second real, minimal upstream patch
   (`0002-include-grp-h.patch`) adding the missing `#include <grp.h>` —
   not PureUnix-specific, a real bug on any libc with strict header
   separation.
10. **`QT_STATFSBUF`/`QT_STATFS`/`ST_RDONLY` (QStorageInfo, disk-space
    reporting) wanted `struct statvfs64`/`::statvfs64`.** PureUnix already
    has a real `statvfs()` (per `user/newlib_compat/sys/statvfs.h`'s own
    comment, backed by a `SYS_STATFS` syscall) — but that syscall and its
    userland implementation turn out **not to actually exist anywhere in
    the current tree** (a stale comment/reference, similar in kind to
    other lost-work gotchas recorded in project memory — not something
    this port needed to resolve, since only the *declaration* is needed
    for `libQt6Core.a` to compile; nothing in this build's scope calls it
    yet). Fixed by extending the existing compat header with a
    `struct statvfs64` + `statvfs64()` declaration (thin duplicate of the
    existing `statvfs` shape) and `ST_RDONLY`, declaration-only, matching
    the pre-existing sibling declarations.
11. **Qt Core's own `<time.h>`-dependent `QElapsedTimer` needs
    `clock_gettime(CLOCK_MONOTONIC, ...)`**, which newlib only declares
    (function *and* the `CLOCK_MONOTONIC`/`CLOCK_REALTIME` macros
    themselves) behind an `#ifdef __rtems__` guard (no case for a
    generic/unknown target). This is genuinely the Phase 4 blocker called
    out in section 4 above, arriving early because Qt Core's own build
    needs it. Real implementations (not stubs) were added to
    `user/newlib_syscalls.c`: `clock_gettime(CLOCK_MONOTONIC, ...)` backed
    by the existing `SYS_GET_TICKS_MS` (PIT-derived, 10ms resolution —
    exactly the clock `QElapsedTimer` needs), `clock_gettime(CLOCK_REALTIME,
    ...)` reusing `gettimeofday()`'s existing `SYS_GETTIMEOFDAY` path, and
    matching `clock_getres()`. Both the function declarations *and* the
    `CLOCK_MONOTONIC`/`CLOCK_REALTIME` macro values were added to
    `user/newlib_compat/time.h` — the macros matter independently of the
    functions: `qelapsedtimer_unix.cpp` guards its entire real-clock path
    behind `#ifdef CLOCK_MONOTONIC` and silently falls back to a cruder
    `gettimeofday()`-based timer without it (a first pass that added only
    the function prototypes compiled fine but silently left Qt on the
    fallback path — caught by checking that `qelapsedtimer_unix.cpp.o`
    actually references `clock_gettime` as an undefined symbol after
    rebuilding, which it now does). No kernel change — this is exactly the
    "no new kernel syscall, just newlib glue" outcome section 4 predicted.

Two more real fixes landed directly in `user/newlib_syscalls.c` while
preparing Phase 4 (Qt Core's event dispatcher, section 4's other blocker):
**`poll()`/`select()` previously ignored their `timeout` argument entirely
and returned "0 ready" immediately regardless.** Qt's own event
dispatcher (`qeventdispatcher_unix.cpp`, part of the Phase 3 build above)
calls `poll(NULL, 0, timeout_ms)` as its "sleep until the next due
QTimer" idiom whenever nothing else is pending — with the old behavior
this would have busy-spun at 100% CPU, firing timers far more often than
requested, the exact "do not busy-spin the Qt event loop" failure mode
the task brief calls out. Both functions now genuinely call `nanosleep()`
(the same real blocking primitive everything else already uses) for the
requested duration whenever nothing is ready, before returning "timed
out" — real fds requested still report ready immediately as before (not
genuine multiplexing, but non-blocking is safer than incorrectly blocking
forever on a real fd this kernel has no way to watch asynchronously); an
infinite (`-1`) timeout is approximated as a bounded repeating 50ms sleep
rather than one true unbounded wait, since there's no real async wakeup
source to watch for here. Verified no regression: `htop` (an existing
real `poll()`/`select()` consumer) still renders and runs correctly in
QEMU after this change, and the full `systest`/`cxxtest` baselines are
unchanged.

`tools/build-qt.sh` runs the complete two-stage build (native host build
for `moc`/`rcc`, then the real cross-compile using `QT_HOST_PATH`) and
vendors the result — see that script and `tools/pureunix-qt-toolchain.cmake` for
the authoritative, fully-commented version of everything above. No
regressions: the existing `make` build, `cxxtest`, and the `systest`
baseline were not touched by anything in this phase (Qt's build lives
entirely in a separate scratch tree via `tools/build-qt.sh`, only vendored
output under `third_party/qt/` and two small newlib_compat/newlib_syscalls
additions landed in the real repo tree).

## Phase 4 results: Qt Core running natively on PureUnix (done, QEMU-verified)

`user/qtcoretest.cpp` — real, unmodified upstream Qt 6 API only: `QString`,
`QByteArray`, `QList`, `QFile` (real VFS I/O, write-then-read-back),
`QElapsedTimer` (the real `CLOCK_MONOTONIC` path), a genuine moc-generated
`QObject::connect()` signal/slot pair, and a real running
`QCoreApplication::exec()` event loop driven by three staggered
`QTimer::singleShot()` calls proving ordering and repeated dispatch, not
just one lucky callback — **passes all 20/20 checks** booted from a real
PureUnix disk image in QEMU (`tools/vt-scripts/run-qtcoretest.txt`,
verified via `make run-test` alongside the rest of this repo's regression
suite with zero regressions: `cxxtest`, `systest` (343/345, the same 2
pre-existing known console-geometry failures — see project memory),
`ash-job-control`, both Ctrl+C/Ctrl+\ tests, `proc-ps-top`, and the VT
raw-switch test all still pass unchanged).

Getting from "Qt Core links" (Phase 3) to "Qt Core actually runs" required
finding and fixing real bugs at every layer — build wiring, general POSIX
gaps, and two genuine architectural gaps neither Phase 1's audit nor Phase
3's build-time work had surfaced (both only show up at real
execution/link time, not at Qt's own `STATIC_LIBRARY`-only `try_compile`
checks):

1. **`tools/build-qt.sh`'s `WORK_DIR` broke under a real link** (not
   during Phase 3, which never needed one — `CMAKE_TRY_COMPILE_TARGET_TYPE
   STATIC_LIBRARY` sidesteps it, see Phase 3 above). qtbase's own top-level
   `CMakeLists.txt` refuses a build directory reached through a symlink
   (`qt_internal_check_if_path_has_symlinks`) — macOS's `/tmp` is itself a
   symlink to `/private/tmp`, so `mktemp -d /tmp/...` needs its real
   physical path (`pwd -P`) before use. Fixed once, in the script, not
   worked around per-invocation.
2. **`tools/qt-toolchain.cmake` was renamed to
   `tools/pureunix-qt-toolchain.cmake`.** A genuine upstream qtbase bug:
   `cmake/QtAutoDetect.cmake`'s own cyclic-toolchain guard checks
   `CMAKE_TOOLCHAIN_FILE MATCHES "/qt.toolchain.cmake$"` — a CMake regex,
   where the unescaped `.` is a wildcard matching *any* character,
   including our toolchain file's own `-` in `qt-toolchain.cmake`. A pure
   naming collision with Qt's own reserved `qt.toolchain.cmake` (the file
   qmake-based application builds generate) had nothing to do with
   PureUnix specifically — renaming was the clean fix, not a patch to
   qtbase's own (correct, if overly broad) guard.
3. **Qt's public headers need the same three consumer-side flags used to
   build Qt itself.** `qfloat16.h`'s use of `_Float16` (Phase 3 item 5)
   and `Q_OS_PUREUNIX` (Phase 3 item 1) both depend on preprocessor state
   that only existed inside `tools/pureunix-qt-toolchain.cmake` during
   Qt's own build — any real *consumer* `#include`ing Qt's headers needs
   `-D__PUREUNIX__ -U__FLT16_MAX__ -D_GNU_SOURCE` too (mirrored into the
   Makefile's new `QT_CONSUMER_FLAGS`, the hand-rolled-Makefile equivalent
   of what `mkspecs/pureunix-g++/qmake.conf` already provides for a
   qmake-based application build). Caught immediately as a real compile
   error, not a silent gap.
4. **`libQt6Core.a` had zero real `blake2b_init`/`blake2s_init`/etc.
   linked in anywhere** (`QCryptographicHash`'s bundled BLAKE2
   implementation) — a real Phase 3 build-configuration bug that only
   surfaces at full-executable link time (`CMAKE_TRY_COMPILE_TARGET_TYPE
   STATIC_LIBRARY` never exercises a real link, see Phase 3's own
   "biggest risk" writeup for why this class of bug hides that long).
   Root cause: qtbase's `find_package(Libb2)` uses `pkg_check_modules`,
   which — unlike `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY/INCLUDE` — isn't
   sysroot-scoped here, so it silently found the *host* machine's own
   libb2 (if installed) and reported `QT_FEATURE_system_libb2=ON`, never
   actually linking anything real for the i686 target. Fixed with
   `-DFEATURE_system_libb2=OFF` in `tools/build-qt.sh` (both build
   stages), forcing qtbase to compile its own bundled BLAKE2 reference
   implementation instead — the same "use the bundled copy, not an
   unreliable system one" treatment already applied to
   zlib/libpng/libjpeg/freetype/harfbuzz/pcre2 in Phase 3. Required a
   real (if partial — target stage only, reusing the persisted host
   `moc`/`rcc`) rebuild of `third_party/qt/i686-elf/`.
5. **27 of the 28 headers under `user/newlib_compat/` had no `extern "C"`
   guard.** A real, general, pre-existing gap in this repo's newlib
   compatibility layer — every one of these headers (`sys/mman.h`,
   `dirent.h`, `poll.h`, `time.h`, `unistd.h`, `sys/stat.h`, ...)
   predates any C++ consumer ever existing in this codebase (`cxxtest.cpp`
   never needed any of them), so the gap was invisible until Qt Core's own
   C++ sources (`qresource.cpp`, `qfsfileengine_unix.cpp`,
   `qfilesystemiterator_unix.cpp`, `qcore_unix.cpp`, `qthread_unix.cpp`,
   `qelapsedtimer_unix.cpp`, `qrandom.cpp`, `qlockfile_unix.cpp`, ...)
   `#include`d them and got C++ name-mangled linkage for `mmap`/`opendir`/
   `poll`/`clock_gettime`/... instead of the plain C symbols
   `user/newlib_syscalls.c` actually exports — real, silent "undefined
   reference" link failures with a demangled-looking signature in the
   error that made the root cause non-obvious at first. Fixed by wrapping
   each header's body in `#ifdef __cplusplus extern "C" { ... }`, matching
   the convention every genuine newlib header (e.g.
   `third_party/newlib/i686-elf/include/dirent.h`) already uses — a
   general newlib-compat-layer fix, not a Qt-specific patch, that benefits
   any future C++ port on this target. `regex.h` was already safe (uses
   newlib's own `__BEGIN_DECLS`/`__END_DECLS` macros). Verified no
   regression: the full existing `make` build (every C program) is
   unaffected — `extern "C"` is a no-op for C compilation.
6. **Five real, previously-unimplemented POSIX functions were genuinely
   required to link**, not just declared: `truncate()` (path-based
   wrapper over the existing `open()`+`ftruncate()`+`close()`),
   `getpagesize()` (a real, honest constant — this kernel's VMM only ever
   uses 4096-byte pages, no huge-page support exists to make it
   configurable), `flock()` (BSD `flock()` reimplemented over the real
   POSIX `fcntl()` advisory locking already added for the SQLite port —
   the same "implement the BSD entry point over the POSIX one" most libcs
   use, not a PureUnix shortcut), `futimens()` (a genuine new kernel
   syscall, `SYS_FUTIME` — an fd-based analog of the existing `SYS_UTIME`,
   built by mirroring how `SYS_FCHMOD`/`SYS_FCHOWN` already resolve an fd
   to its `open_file_t.path` and defer to the same `vfs_utime()` a
   path-based call uses), and `getentropy()` (real hardware entropy via
   the unprivileged `RDRAND` instruction when `CPUID.1:ECX.30` reports it
   available — the same "just execute it, no ring-0 needed" reasoning
   `kernel/vmm.c`'s own `cpuid`-based PAT probe already relies on — with a
   tick-seeded xorshift fallback on a CPU/QEMU model without it; real
   entropy quality isn't claimed without `RDRAND`, honestly reflecting
   this target's actual non-cryptographic `QRandomGenerator` use, not a
   security-sensitive CSPRNG deployment).
7. **`statvfs64()` (QStorageInfo's `QStorageInfoPrivate::retrieveVolumeInfo()`)
   needed a real backing implementation**, not just the declaration Phase
   3 item 10 added — a real, previously entirely-absent `SYS_STATFS`
   syscall (`fs/vfs.c`'s new `vfs_statfs()`, dispatched to a real
   `ext2_statfs()` in `fs/ext2/mount.c` that sums `bg_free_blocks_count`/
   `bg_free_inodes_count` across every block group — the same live,
   write-path-maintained BGDT counters `fs/ext2/write.c` already updates
   on every allocation/free, not a separate/stale tracker). FAT16/procfs
   leave `vfs_ops_t.statfs` `NULL` (no free-space bookkeeping tracked
   there), correctly failing `-ENOSYS` rather than fabricating a number.
   `statvfs()`/`fstatvfs()`/`statvfs64()` (`user/newlib_syscalls.c`) are
   real now; `fstatvfs()` alone stays a real, honest `-ENOSYS` (no
   fd-to-path resolution syscall exists for statfs specifically, and
   nothing in this port's actual scope — `QStorageInfo` always goes
   through the path-based `statvfs64()` — ever calls it).
8. **`tools/mkext2.py`'s `NUM_GROUPS` needed to grow from 3 (24 MB) — but
   grew too far on the first attempt (12 groups, 96 MB) and caused a real,
   confusing regression**: every VT failed to find `/bin/sh` at boot,
   each falling back to the built-in recovery shell, whose different
   banner text then made `tools/vt-inject-test.py`'s own boot-detection
   `wait_for(r"Enter 'help'", ...)` time out too — two layers of symptom
   stacked on top of the real root cause. That root cause: `root.img`
   (the whole EXT2 image) travels as a **whole-file GRUB module** loaded
   into RAM for `tools/vt-inject-test.py`'s `LIVE_ISO` boot path, which
   only gives QEMU 128 MB total (`-m 128M`) shared with `fat.img` (32 MB)
   + the kernel itself — 96 MB pushed the combined GRUB module load
   straight through that ceiling (`error: out of memory`, a real GRUB-level
   error printed *before* the kernel even started, confirmed by capturing
   the complete, untruncated serial log rather than trusting the
   already-misleading `/bin/sh`-not-found symptom two layers downstream).
   No real OS regression at all — settled on `NUM_GROUPS = 6` (48 MB),
   comfortable headroom for `qtcoretest.elf` (~5-7 MB) without coming
   anywhere near that combined ~96 MB ceiling.
9. **The first real Qt Core run took a `KERNEL PANIC: CPU exception 14
   (page fault) ... cr2=0x0`, immediately on `QCoreApplication`
   construction** — `%gs:0x0` (a `mov %gs:0x0,%eax` compiled into
   `QBindingStorage`'s constructor, confirmed by disassembling the exact
   faulting `eip`) reading from linear address 0, because **PureUnix had
   no TLS (thread-local storage) support at all** — Phase 1's own audit
   had flagged `.tdata`/`.tbss` as unimplemented but judged it "out of
   scope, no current userspace need"; Qt6's `QBindingStorage` (used by
   every `QObject`, part of Qt6's new bindable-properties machinery, not
   gated behind `FEATURE_thread`) turned out to be that need, arriving
   exactly when Phase 1 predicted a *different* thing (threading) might,
   not thread-local storage itself. Implemented for real, not stubbed —
   the smallest correct version for this kernel's single-CPU,
   cooperatively-scheduled design (i386 Variant II TLS, "local exec"
   model, the only one that makes sense for a statically-linked
   executable with no dynamic loader):
   - `user/linker.ld`: explicit `.tdata`/`.tbss` output sections with
     `__tdata_start`/`__tdata_end`/`__tbss_start`/`__tbss_end` symbols,
     `.tdata` immediately followed by `.tbss` (the exact contiguous
     layout `ld`'s own `R_386_TLS_LE` relocation offsets — the compiled
     `%gs:-0x18`-style constants — assume).
   - `user/newlib_crt0.c`'s new `tls_init()`, called first thing in
     `_start_c()` (before even `__register_frame()`, since any global C++
     constructor could in principle touch a `thread_local`): allocates a
     block sized to the `.tdata`+`.tbss` template, copies `.tdata`, zeros
     `.tbss`, appends a self-pointing TCB word (`*tp = tp`, what `%gs:0`
     reads — the Variant II ABI's defining trick), tells the kernel about
     it, then loads `%gs` with the new selector directly (no privilege
     needed for that half — only defining the underlying descriptor did).
   - `include/pureunix/syscall.h`'s new `SYS_SET_TLS`: a task-local
     `tls_base` (`include/pureunix/task.h`) plus a real, minimal, one-entry
     GDT addition — `arch/i386/gdt.c`'s new gate 6 (selector `0x33`) and
     `gdt_set_tls_base()`, repointed at whichever task is about to run by
     `kernel/task.c`'s `task_yield()` context switch (this kernel is
     single-CPU/cooperatively-scheduled, so one shared descriptor updated
     at switch time is race-free — no per-CPU GDT needed). Inherited as-is
     across `fork()` (the whole address space, TLS block included, is
     already deep-copied — `kernel/task.c`); reset to 0 across `exec()`
     (`kernel/elf.c`) since a fresh image has no TLS block of its own yet.
   - A general C11/C++11 `thread_local` runtime primitive now, not a
     Qt-specific hack — any future PureUnix program using `thread_local`
     gets this for free.
10. **Even with TLS fixed, `QCoreApplication::exec()` still hung
    indefinitely** past the 18th of 20 checks (every synchronous check —
    `QString`/`QByteArray`/`QList`/`QFile`/`QElapsedTimer`/the real
    signal/slot connection — passed; only the event-loop-dependent
    `QTimer` checks never ran). Root cause: `poll()`
    (`user/newlib_syscalls.c`) **lied** — it reported every real fd passed
    to it as immediately ready, regardless of whether there was actually
    any data, a "close enough" approximation that happened to work for
    every previous caller (ash's `read -t`, `libbb`'s `safe_poll()` retry
    wrapper, both of which just do a real blocking `read()`/`write()`
    right after and had always had real data waiting by then in practice).
    Qt's `QEventDispatcherUNIX` includes its own same-process wakeup pipe
    in its `poll()` calls; told it was instantly "ready" with nothing
    actually written to it yet, it called a real, genuinely-blocking
    `read()` on that pipe and never returned. The Phase 3 fix (`poll(NULL,
    0, timeout)` → real `nanosleep()`) only ever covered Qt's *other*
    idiom (zero fds, "just sleep until the next timer") — a real,
    different bug, not a leftover of the same one. Fixed with a genuine
    new kernel primitive, **`SYS_POLL`** (`arch/i386/syscall.c`) — exactly
    the "smallest correct general primitive" Phase 1's audit (section 4)
    had already anticipated needing, just for a more specific reason than
    predicted: for `FD_KIND_PIPE` fds (the one kind this kernel can check
    for real without new infrastructure — `pipe_buf_t.count`, already
    real and already maintained by every `read()`/`write()`/`close()`),
    it checks genuine buffer occupancy instead of lying, and blocks for
    real (repeated short `pit_sleep()` polling, `arch_enable_interrupts()`
    first per the documented interrupt-gate-blocking rule — the same
    precedent `SYS_PING`'s `icmp_ping()` already established) up to the
    requested timeout when nothing is ready yet, rather than claiming
    instant readiness. Every other fd kind (regular file/tty/pty/procfs)
    keeps the previous optimistic-readiness answer unchanged — no
    readiness tracking exists for those yet, and nothing in this port's
    current scope depends on it (real wait-queue-based wakeup instead of
    short-interval polling, and covering pty/tty too, are natural
    follow-ups once Phase 5's external PUDE GUI client protocol needs
    them). `user/newlib_syscalls.c`'s `poll()`/`select()` are now real,
    complete rewrites around this syscall (`struct pu_raw_pollfd` wire
    struct) rather than the old always-ready stub — verified no
    regression via the same `htop`/`systest` real `poll()`/`select()`
    consumers Phase 3's fix had already checked.

`Makefile`'s new `QT_DIR`/`QT_HOST_TOOLS`/`MOC`/`QT_CORE_CFLAGS`/
`QT_CORE_LIBS`/`QT_PROGRAMS`/`QT_ELFS` wire `user/qtcoretest.cpp` into the
real build (moc step, compile, link) following the exact same pattern as
the existing `NEWLIB_CXX_PROGRAMS` (`cxxtest`) — `build/qt-host-tools`
(the persisted native-host `moc`/`rcc`, `tools/build-qt.sh`'s own output,
gitignored like every other build artifact) is the one new external
prerequisite. `tools/vt-scripts/run-qtcoretest.txt` follows
`run-cxxtest.txt`'s exact convention, and is now part of `make run-test`'s
regression suite alongside every other real boot test in this repo.

Next when resuming: Phase 5 (QtGui — the platform-independent parts:
`QImage`, `QPainter`'s raster backend, `QColor`, font metrics without a
real windowing backend yet) is the natural next increment per the port's
own "build incrementally, verifying each stage" mandate, followed by the
PureUnix QPA platform plugin and the external PUDE GUI client protocol
(section 5 above) once there's an actual framebuffer surface for QtGui to
draw into.
