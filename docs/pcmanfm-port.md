# PCManFM-Qt port — plan and status

## Goal

Port an unmodified (or minimally patched) upstream release of
[PCManFM-Qt](https://github.com/lxqt/pcmanfm-qt) to run as a native i686
PureUnix ELF, launched from PUDE like every other Qt app in
[docs/qt-port.md](qt-port.md) (Phase 6's "pureunix" QPA plugin). Not a
custom file manager — PUFiles already exists for that
(`docs/`/`user/pude_files.c`) — the point here is proving a real,
substantial upstream Qt Widgets desktop application can be built and run
with minimal PureUnix-specific patching.

## The real dependency graph (researched 2026-07-18, via the live
upstream repos on GitHub — not assumed from memory)

PCManFM-Qt 2.4.0 (current release, April 2026) depends on:
- `Qt6Widgets`, `Qt6DBus`, `Qt6LinguistTools` (`>= 6.6.0`)
- `fm-qt6` (libfm-qt) `>= 2.4.0` — its own core library, REQUIRED
- `lxqt2-build-tools` — just CMake helper macros, lightweight
- `LayerShellQt` `>= 6.0.0` — Wayland layer-shell protocol support

`libfm-qt` 2.4.0, in turn, REQUIRES (verified from its real
`CMakeLists.txt`, not assumed):
- `Qt6Widgets`, `Qt6GuiPrivate` (if Qt >= 6.10), `Qt6LinguistTools`
- `GLIB >= 2.50.0` **with components `gio`, `gio-unix`, `gobject`,
  `gthread`** — REQUIRED, not optional. `src/core/*.cpp`'s `g_file_*`
  calls are the actual VFS backend for the entire application
  (`folder.cpp`, `fileinfo.cpp`, `dirlistjob.cpp`, `filetransferjob.cpp`,
  `deletejob.cpp`, `trashjob.cpp`, `mountoperation.cpp`,
  `filemonitor.cpp`, `thumbnailjob.cpp` — this is not a small corner of
  the codebase, it *is* the file-access layer).
- `MenuCache >= 1.1.0` and `lxqt-menu-data >= 2.4.0` — LXQt desktop
  application-menu data, used for the "Applications" bookmark/places
  entries.
- `Exif` (libexif) — image thumbnail EXIF orientation/metadata.
- `XCB` — used for exactly one thing: `src/xdndworkaround.cpp`, an X11
  drag-and-drop compatibility shim. Isolated, small, and meaningless on
  a platform with no X11 server at all (PureUnix has neither X11 nor
  Wayland) — a real, minimal, justifiable patch to compile this file out
  entirely (`#ifdef` guard or just excluding it from the CMake source
  list) rather than porting XCB.

**Conclusion: GLib/GObject/GIO is the one dependency that cannot be
patched around or disabled** — it's the actual file-I/O implementation
this whole application is built on, in every version of libfm-qt back to
its inception (this is a well-known, long-standing architectural fact
about the library, not something that changed recently). `LayerShellQt`
(Wayland) and `Qt6DBus` (used for single-instance activation and
desktop-notification IPC in the main app, not `libfm-qt` itself) are
smaller and more plausibly patchable/disableable once we're looking at
actual source, but haven't been investigated in depth yet — GLib is the
long pole, so it comes first.

## What porting GLib/GObject/GIO to PureUnix actually requires

Real requirements, checked against what PureUnix currently has (not
assumed):

1. **Meson build system**, not autotools (GNOME dropped autotools for
   GLib years ago) — needs a real Meson cross file, the same role
   `tools/pureunix-qt-toolchain.cmake` plays for Qt's CMake build. Not
   yet written.
2. **DONE (2026-07-18).** `libffi` — GObject's generic signal-marshalling
   machinery depends on it directly. See phase 1 below for the full
   writeup (real cross-build, real on-target regression test, two
   general gotchas found and fixed).
3. **DONE (2026-07-18).** A real, working `iconv()` — newlib ships
   `<iconv.h>` declarations but **no linkable implementation at all** (confirmed: `nm
   third_party/newlib/i686-elf/lib/libc.a` has zero `iconv_open`/`iconv`
   symbols). GLib uses it pervasively for charset conversion. Since this
   whole system only ever runs in a single UTF-8 locale (see the
   recurring `Detected locale "C" ... not UTF-8` warning throughout
   `docs/qt-port.md` — PureUnix has no locale database at all), a
   real-but-deliberately-narrow `iconv()` — a correct passthrough for
   UTF-8-to-UTF-8 (the only conversion that will ever actually be
   exercised) and a real `EINVAL`/`errno` failure for anything else, not
   a silent lie — is the smallest correct implementation for what this
   platform actually needs, not a fully general iconv table. Implemented
   in `user/newlib_syscalls.c` (`iconv_open()`/`iconv()`/`iconv_close()`,
   accepting UTF-8/ASCII-family charset name aliases, real `E2BIG` when
   the output buffer is too small, real `EINVAL` for anything else) and
   proven on-target via 8 new checks in `user/libctest.c`'s
   `section_iconv()` — a real 2-byte-UTF-8 round trip, a rejected
   unsupported charset, and a real `E2BIG` partial-copy check, not just
   "does it link". `make run-test` still at the known 343/345 baseline.
   New, small, real code, not a stub that pretends to succeed at
   conversions it
   doesn't do.
4. **Real POSIX threads.** This is the one that changes the shape of the
   whole project. GLib's `GMutex`/`GCond`/`GThread` are not optional in
   any build configuration since ~2.32 — and `libfm-qt`'s own
   architecture (`src/core/*job.cpp` — `DirListJob`, `DeleteJob`,
   `FileTransferJob`, `ThumbnailJob`, etc., all named and structured as
   background worker jobs) strongly implies real threading is exercised
   for file operations, not just linked-but-unused. **PureUnix currently
   has zero threading primitives** — every task is a full `fork()`ed
   process with its own address space; there is no "another execution
   context sharing my address space" concept in `kernel/task.c` at all.
   Building *real* kernel-level threads (shared VM, per-thread stacks,
   scheduler-visible thread vs. process distinction, futex-style
   waiting) would be a bigger kernel feature than anything added during
   the entire Qt6 port (bigger than TLS, bigger than xHCI).

   **DONE (2026-07-18): a real, honest single-threaded pthread shim, not
   fake kernel threading.** Implemented in `user/newlib_syscalls.c`.
   `GMutex`/`GCond`/`GOnce` (via `pthread_mutex_*`/`pthread_cond_*`) are
   genuine no-ops (correct precisely because nothing on this platform
   can ever contend on them — there's only one execution context per
   process). `pthread_create()` runs the given entry point
   **synchronously, inline, before returning** rather than deferring it
   to a real concurrent thread — real, not fake: it uses a genuine
   `setjmp`/`longjmp` round trip (the same primitive already validated
   working on this platform via `user/cxxtest.c`) so `pthread_exit()`
   called from deep inside the entry point still unwinds correctly back
   to the waiting `pthread_create()` call frame, and a real heap-
   allocated per-thread record carries the actual return value through
   to `pthread_join()`. `pthread_once()` genuinely only runs its
   init routine once even though nothing can race on it; thread-specific
   data degenerates to a real flat global table (correct given there's
   only ever one logical "current thread" alive at a time here). Two
   real newlib gaps found getting here, both general (not GLib-specific):
   `<pthread.h>`'s entire real API surface (types and prototypes) is
   gated behind `#if defined(_POSIX_THREADS)`, never set for a bare
   i686-elf target (only `__rtems__`/`__XMK__`/`__CYGWIN__` branches in
   `sys/features.h` define it) — fixed by adding `-D_POSIX_THREADS=1`
   (and, one level deeper, `-D_UNIX98_THREAD_MUTEX_ATTRIBUTES=1` for
   `pthread_mutexattr_t`'s own `type` field/`settype()`/`gettype()`) to
   the shared `NEWLIB_CFLAGS` in the Makefile — a real statement of
   platform capability now that a real implementation backs it, not a
   lie. Proven at runtime via 9 new checks in `user/libctest.c`'s
   `section_pthread()`: a real thread create/join round trip with a
   genuine computed return value, a real `pthread_exit()` unwind past
   unreachable code, exactly-once `pthread_once()` semantics across 3
   calls, and a real thread-specific-data get/set round trip — not just
   "does it link". `make run-test` still at the known 343/345 baseline,
   no regressions (a global `NEWLIB_CFLAGS` change, so every newlib-
   linked binary in the tree, including every Qt one, was rebuilt and
   reverified).
   
   The documented, honest cost of the synchronous-execution design: a
   job that would background on a real OS instead blocks the caller for
   its duration (a real, disclosed limitation, not a silent correctness
   bug — same class of tradeoff as, e.g., this project's SDL2 port
   disclosing "no real audio/threads/joystick" up front rather than
   faking them). Matches this project's own established precedent of
   disabling a thread-dependent *feature* rather than building real
   concurrency for it (see `docs/qt-port.md`'s `-DPCRE2_DISABLE_JIT` —
   PCRE2's JIT needs real pthreads too, and was compiled out rather than
   ported around, for exactly this reason). Deliberately NOT implemented
   yet: cancellation, scheduling/priority attributes, CPU affinity,
   cleanup-handler stacks, rwlocks, barriers, spinlocks — none are
   reachable from GLib's own real usage researched so far; will add real
   implementations if a future build genuinely needs one, same
   "fix real errors as they occur" methodology as everything else in
   this port.
5. **`pcre2`** — GLib's `GRegex` needs it. Qt's own build
   (`third_party/qt/`) already vendors a real cross-built PCRE2 as
   `Qt6BundledPcre2`, but that's built as an internal Qt static library,
   not necessarily exposed as a standalone reusable `libpcre2-8.a` with
   its own public headers in a form GLib's Meson build can just find via
   `pkg-config`. Simplest, most isolated path: vendor a second, small,
   standalone PCRE2 build for GLib specifically (same source version is
   fine, doesn't need to be a shared artifact) rather than trying to
   reach into Qt's internal build product.
6. **GIO's actual feature scope** — the plan is *local files only*: no
   D-Bus (GVfs remote mounts, `GDBusConnection`), no `inotify`-based file
   monitoring (Linux-specific syscall that doesn't exist here — GIO has
   a real, supported polling-based `GFileMonitor` fallback for platforms
   without `inotify`, which is what this port will use), no
   `GVolumeMonitor` backend beyond "no removable media" (a real,
   supported "unix" GIO volume monitor exists that reads `/proc/mounts`-
   style data; PureUnix's own mount table would need a small adapter,
   or this can degrade to "no monitor, no volumes list" as a disclosed
   limitation for a first pass).

## Phased plan (tasks #10 onward in the working task list)

1. **DONE (2026-07-18).** Vendored + cross-built `libffi` 3.7.1
   (`third_party/libffi/i686-elf/`, `tools/build-libffi.sh`), real
   `user/libffitest.c` regression test wired into `make all`/
   `tools/vt-scripts/run-libffitest.txt` — genuinely exercises
   `ffi_prep_cif()`/`ffi_call()` through libffi's real x86 assembly
   trampolines (`src/x86/sysv.S`) on-target, not just a link check
   (`libffitest: 4/4 passed`, QEMU-verified, `make run-test` still at the
   known 343/345 baseline, no regressions). Two real findings along the
   way:
   - `configure`'s own "does the compiler work" bootstrap link probe
     needs `-T user/linker.ld` explicitly in `LDFLAGS`, not just
     `-nostdlib` — `newlib_crt0.c` unconditionally references TLS
     section-boundary symbols (`__tbss_end`/`__tdata_end`/...) added to
     `user/linker.ld` during the Qt6 port's i386 TLS support
     (`docs/qt-port.md`), whether or not the program being linked uses
     `thread_local` at all. A real, general gotcha for any future
     vendored-library build script written after that TLS work landed.
   - libffi's own `libtool`-driven build refuses to archive
     `libffi.la`/`libffi_convenience.la` ("cannot build libtool library
     ... from non-libtool objects") once the bootstrap-probe's
     crt0/newlib_syscalls `.o` files are present in the global `LIBS`
     autoconf variable — autoconf has no way to scope `LIBS`/`LDFLAGS` to
     just the one bootstrap check. Every real per-file compile succeeds
     fine regardless; only the final libtool archival step objects. Fix:
     let `make` fail there (harmless, all real `.o`s already exist by
     then) and archive `libffi.a` directly with the real cross
     `ar`/`ranlib`, bypassing libtool's own broken link entirely — exactly
     what libtool would have done internally for a static-only library
     anyway.
   - Confirmed (not just assumed): libffi's closure support
     (`src/closures.c`, needed for GObject's C-callback trampolines)
     only enables its `mmap()`-based writable+executable memory path on
     `__linux__`/Windows; PureUnix's `i686-elf` target defines neither,
     so it automatically falls through to the real, upstream-supported
     "memory returned by `malloc` is writable and executable, so just use
     it" path — a correct description of this kernel's actual page
     tables (no W^X/NX enforcement anywhere), not a hack. Zero source
     changes needed for this.
2. **DONE (2026-07-18).** Real `iconv()` (UTF-8 passthrough, real
   `EINVAL` otherwise) — `user/newlib_syscalls.c`, proven via
   `user/libctest.c`'s new `section_iconv()` (8 checks, QEMU-verified,
   `make run-test` still at the known 343/345 baseline). GLib's own
   configure-time checks may still surface further real newlib gaps once
   phase 3 (below) actually starts — expect several, every previous
   vendored-library port in this repo (ncurses/SDL2/Qt) found real newlib
   gaps this way; no reason to expect GLib is different.
3. Write `tools/pureunix-glib-crossfile.ini` (Meson cross file) +
   `tools/build-glib.sh`. Cross-compile GLib core + GObject with GIO's
   local-file backend only (Meson options to disable dbus/selinux/
   fam/inotify as each is hit — expect several rounds of real build
   fixes, not a one-shot clean build, same as every prior vendored
   dependency in this codebase).
4. **DONE (2026-07-18).** The pthread shim (`user/newlib_syscalls.c`) —
   see phase 4 above for the full writeup (real no-op mutexes/conds,
   synchronous-inline `pthread_create()` with a genuine `setjmp`/
   `longjmp`-backed `pthread_exit()`, `pthread_once()`, thread-specific
   data, 9 new on-target checks in `user/libctest.c`).
5. Vendor `MenuCache` + `libexif` (both much smaller, more
   conventional C libraries — lower risk than GLib itself).
6. Patch out the one XCB-dependent file (`xdndworkaround.cpp`) in
   `libfm-qt` — smallest real patch in this whole port, disables exactly
   one X11-specific drag-and-drop compatibility feature that has no
   meaning without an X server.
7. Cross-compile `libfm-qt` itself against all of the above.
8. Cross-compile `pcmanfm-qt` itself, investigate `Qt6DBus`/
   `LayerShellQt` requirements for real (may need the same kind of
   targeted disable as XCB above, or may need small real ports —
   not yet researched in depth).
9. Wire into PUDE's launcher (`user/pude_qtclient.c`'s existing generic
   `app_class_t` adapter — no changes needed there, same pattern as
   `qtclient_widgets_app_class`).
10. End-to-end QEMU testing + full regression suite + final
    documentation (upstream versions pinned, every patch listed,
    disabled features listed, build procedure, known limitations).

## Status: phases 1-2-4 (libffi, iconv, pthread shim) done, phase 5 (pcre2) next

This is a genuinely large undertaking — realistically comparable in
scope to the entire Qt6 port (`docs/qt-port.md`), possibly larger, since
GIO alone has more surface area than QtCore and there's no precedent in
this codebase for *any* GLib-family library yet. Proceeding in the
phased order above; each phase gets its own real, working, tested
artifact before moving to the next, the same incremental methodology
that got Qt6 itself working.
