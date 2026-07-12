# Chocolate Doom (vendored)

[Chocolate Doom](https://www.chocolate-doom.org) 3.1.1 source
(GitHub `chocolate-doom/chocolate-doom`, tag `chocolate-doom-3.1.1`,
tarball sha256
`034afdd8a22a35783b6ffbf2004732d9654b1ca21843ebc563566335653c5dee`),
vendored as an extraction of the upstream release source archive under
`chocolate-doom-3.1.1/`. No existing upstream file is patched — see
`docs/chocolate-doom-port.md` for the full platform port and "Why no
patches were needed" below. This file covers just the vendoring choices.

Pruned after extraction (all upstream-provided, none of it read by this
port): `.devcontainer/`, `.github/` (CI/dev-container scaffolding), and
`win32/` (Windows-only `opendir()` shim, irrelevant here — PureUNIX
already has a real `opendir()`/`readdir()`). Everything else — `src/`
(including `heretic/`, `hexen/`, `strife/`, which this port doesn't
compile but keeps vendored for completeness, matching upstream's own
multi-game repository layout), `opl/`, `pcsound/`, `textscreen/`, `data/`,
`man/`, `cmake/`, both build systems (`configure.ac`/`Makefile.am` and
`CMakeLists.txt`) — is untouched, kept for reference even though this
port's build (see below) doesn't invoke either.

Only `chocolate-doom` itself is built (not `chocolate-server`,
`chocolate-setup`, or the Heretic/Hexen/Strife binaries) — see
`docs/chocolate-doom-port.md` for why the setup GUI isn't needed to make
the game genuinely playable.

## Why not Chocolate Doom's own build system

Same reasoning as `third_party/SDL2/README.md`: both of Chocolate Doom's
build systems (autotools and CMake) are built around a hosted target that
probes for installed libraries (`find_package(SDL2_mixer)`, `pkg-config`,
...) and generates `config.h` from a template — neither has a PureUNIX
case, and there's no cross-compiled way to *run* either probing process
here anyway. PureUNIX's top-level `Makefile` instead compiles an explicit,
curated file list — copied directly from `src/CMakeLists.txt`'s own
`COMMON_SOURCE_FILES`/`GAME_SOURCE_FILES`/`DEHACKED_SOURCE_FILES` plus
`src/doom/CMakeLists.txt`'s file list, so this stays a faithful
reproduction of upstream's own real link graph, not a guess — the same
"hand-list the real files, don't hand-run the real build system" pattern
`third_party/htop`/`third_party/SDL2` already established.

`include/config.h` (new, not a patch — Chocolate Doom's own build always
generates this file fresh from `cmake/config.h.cin`, so authoring it by
hand is the equivalent of running that template once with PureUNIX's real
answers, exactly like `SDL_config_pureunix.h` stands in for SDL2's own
generated config) is this platform's answer to that template: no
FluidSynth/libsamplerate/libpng (none exist on PureUNIX), `HAVE_DIRENT_H`
(PureUNIX has a real `opendir()`/`readdir()`), and deliberately no
`HAVE_MMAP` (see "Why no patches were needed").

## Why no patches were needed

Every PureUNIX-specific decision Chocolate Doom needs turns out to already
be a real, upstream-supported configuration, expressed entirely through
`include/config.h`'s `HAVE_*`/`DISABLE_*` defines — nothing in
`chocolate-doom-3.1.1/` itself is modified:

- **`DISABLE_SDL2MIXER=1` / `DISABLE_SDL2NET=1`** — real upstream build
  options (`cmake -DENABLE_SDL2_MIXER=OFF -DENABLE_SDL2_NET=OFF` is a
  documented, supported configuration upstream ships), not a PureUNIX
  invention. `i_sdlmusic.c`/`i_sdlsound.c`/`i_musicpack.c`/`net_sdl.c`
  each already have a real, complete `#ifndef DISABLE_SDL2*` / `#else`
  branch pair — PureUNIX has no `SDL2_mixer`/`SDL2_net` vendored at all
  (no dynamic linker to load them against even if it did — see
  `third_party/SDL2/README.md`'s `SDL_LOADSO_DISABLED` reasoning), so
  this takes the exact same "not supported" branch a real desktop build
  without those libraries installed would.
- **No `HAVE_MMAP`** — `w_file.c`'s `W_OpenFile()` only ever tries
  `posix_wad_file`'s `mmap()`-backed path if the user explicitly passes
  `-mmap` on the command line; the default (and this port's only) path is
  the plain `fopen()`/`fread()`-based `stdc_wad_file` — real, upstream,
  always-compiled, not a fallback invented for this port. PureUNIX's own
  `mmap()` only supports anonymous scratch allocations anyway (see
  `user/newlib_compat/sys/mman.h`), so this is the only real option here,
  and it was always upstream's own default besides.
- **No `HAVE_FLUIDSYNTH`** — `i_flmusic.c`'s entire body is `#ifdef
  HAVE_FLUIDSYNTH`; without it the file compiles to nothing, exactly like
  a desktop build without FluidSynth installed.
- **The always-present `opl`/`pcsound` fallback audio modules still link
  and initialize successfully** even with audio otherwise unavailable:
  they call real `SDL_OpenAudioDevice()`, which succeeds against
  PureUNIX's SDL2 port's dummy audio driver (`SDL_AUDIO_DRIVER_DUMMY` —
  see `docs/sdl-port.md`) exactly like it would on any other platform
  with no real audio hardware — silently producing no sound rather than
  failing, the same honest "audio cleanly unavailable" behavior the SDL2
  port itself already established, inherited here for free.
