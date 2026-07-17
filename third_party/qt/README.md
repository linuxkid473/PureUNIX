# Qt 6 (vendored build output)

Real [Qt 6.5.3](https://download.qt.io/official_releases/qt/6.5/6.5.3/submodules/qtbase-everywhere-src-6.5.3.tar.xz)
`qtbase` (Core + Gui + Widgets only), cross-compiled for i686 PureUnix.
Full upstream source (~280 MB, by far the largest of any port in this
repo) is **not** vendored — only the resulting headers and static
libraries (~65 MB) under `i686-elf/`, the same "vendor a prebuilt build of
real upstream source" pattern already used for `third_party/newlib/` and
`third_party/libstdcxx/`, just via a real CMake build instead of
autotools (see `tools/build-qt.sh`, `tools/pureunix-qt-toolchain.cmake`).

Two real, minimal upstream Qt patches are applied at build time
(`patches/`) — genuine bugs/gaps in qtbase itself, not PureUnix hacks:

1. `0001-add-Q_OS_PUREUNIX.patch` — adds a `Q_OS_PUREUNIX` case to Qt's own
   OS detection (`src/corelib/global/qsystemdetection.h`), mirroring how
   `Q_OS_VXWORKS` already works there (set from the mkspec via a `#define`,
   not autodetected from a compiler builtin — PureUnix has none).
2. `0002-include-grp-h.patch` — a missing `#include <grp.h>` in
   `qfilesystemengine_unix.cpp`, which otherwise relies on `<pwd.h>`
   transitively pulling in `getgrgid()`'s declaration. That happens to work
   on glibc/macOS's libc but not on newlib's more strictly separated
   headers — a real bug on any libc that keeps them apart, not specific to
   this port.

`mkspecs/pureunix-g++/` is PureUnix's first Qt platform mkspec (modeled on
`mkspecs/freebsd-g++`, then stripped of pthread/X11/OpenGL assumptions —
see that directory's own file comments).

See `docs/qt-port.md` for the complete blocker map, every build error
this pinned configuration works around and why, the full enabled/disabled
feature list, and everything from Phase 4 onward (native Qt Core test,
external PUDE GUI client protocol, the PureUnix QPA platform plugin, and
the final Qt Widgets demo).

## Why not qtbase's own top-level build invoked directly from this repo

qtbase's CMake build system is used for real (unlike Pattern-A ports which
hand-list source files in the top-level `Makefile`) — Qt is far too large
and its own build logic far too load-bearing (feature detection, moc/rcc
code generation, module dependency graphs) to reproduce by hand. What
`tools/build-qt.sh` replaces is only *invoking* that build system from
inside this repo's own tree at every `make`; instead it's run once,
offline, cross-compiling against `tools/pureunix-qt-toolchain.cmake`, and only the
resulting static libraries + headers are committed.

## Compile-time configuration

See `tools/pureunix-qt-toolchain.cmake`'s own comments for the full, exact
rationale behind every flag; summarized in `docs/qt-port.md`.
