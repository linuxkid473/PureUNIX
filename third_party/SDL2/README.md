# SDL2 (vendored)

[SDL2](https://www.libsdl.org) 2.28.5 source
(`https://www.libsdl.org/release/SDL2-2.28.5.tar.gz`, sha256
`332cb37d0be20cb9541739c61f79bae5a477427d79ae85e352089afdaf6666e4`), vendored
as an extraction of the upstream release tarball under `SDL2-2.28.5/`. No
existing upstream file is patched — see "Why no patches were needed" below.
`docs/sdl-port.md` covers the full platform port; this file covers just the
vendoring choices.

Pruned after extraction (all upstream-provided, none of it read by this
port): `test/` (SDL's own ~50MiB test-asset suite — PureUNIX has its own
test program instead, `user/sdltest.c`), and the IDE/platform-specific
project directories this target has no use for: `android-project/`,
`Xcode/`, `Xcode-iOS/`, `VisualC/`, `VisualC-WinRT/`, `wayland-protocols/`,
`mingw/`, `Makefile.os2`, `Makefile.w32`, `Makefile.pandora`. Everything
else — `src/`, `include/`, `docs/`, `configure`/`CMakeLists.txt`, the
top-level license/readme files — is untouched, kept for reference even
though this port's build (see below) doesn't invoke them.

## Why not SDL's own build system

SDL2 ships both an autotools `configure` and CMake, and either one *could*
be cross-compiled against — but both are built around probing a hosted
target (`AC_CHECK_FUNC`/`find_library` against pthread, dlopen, X11, ALSA,
...) and then picking from a fixed menu of existing platform backends
(`x11`, `wayland`, `windows`, `cocoa`, `dummy`, ...); neither has a
`pureunix` case to select. Getting either build system to emit something
useful would mean either faking out dozens of feature probes one at a time
(the same class of problem `third_party/ncurses`'s `cf_cv_working_poll`
cross-compile gotcha already demonstrated once) or teaching SDL's own build
files about a platform they don't know exists — a much larger and more
fragile undertaking than the alternative every real "new SDL2 platform
port" (Dreamcast, PSP, Nintendo 3DS, ...) actually uses: **SDL explicitly
supports being built by a platform's *own* build system**, hand-listing
which source files apply, exactly the pattern this project already uses for
TCC/Lua/SQLite. `SDL_config.h` — normally autotools/CMake-generated — is
instead one of the small hand-written templates SDL ships specifically for
this (`include/SDL_config_minimal.h`), authored here as `include/SDL_config.h`
for PureUNIX (see `docs/sdl-port.md`).

PureUNIX's top-level `Makefile` compiles an explicit, curated list of SDL2's
own `.c` files (core, common video/render/events/timer machinery, the
software renderer, and every "dummy"/no-op backend for subsystems this port
doesn't support yet — joystick, haptic, sensor, audio, loadso) plus one new
backend directory this port adds: `src/video/pureunix/` (the real
framebuffer/input glue, `docs/sdl-port.md`). Adding a new platform backend
directory under `src/video/` is the standard, upstream-sanctioned way to
extend SDL to a new target — not a patch to any existing file.

## Why no patches were needed

Every PureUNIX-specific decision SDL2 needs is expressed through
`include/SDL_config.h` (which subsystems compile in at all) and the new
`src/video/pureunix/` backend (how the video/event/timer subsystems that
*are* enabled actually talk to the kernel) — nothing in `SDL2-2.28.5/`
itself is modified. Threads, the dynamic-library loader, joystick/haptic/
sensor, and audio are all compiled out via `SDL_config.h` (see
`docs/sdl-port.md`'s scope notes on what's cleanly unsupported for now vs.
architected to add later) using upstream's own `#ifdef`-gated fallback
paths for each, the same "real, complete, upstream-provided branch of the
code, not a stub" reasoning `third_party/lua/README.md` documents for
`loadlib.c`'s no-dlopen fallback.
