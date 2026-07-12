/* PureUNIX's hand-written answer to what Chocolate Doom's own build would
 * generate from cmake/config.h.cin -- see third_party/chocolate-doom/
 * README.md's "Why not Chocolate Doom's own build system" for why this is
 * authored directly instead of running CMake's configure_file() step
 * (which this cross-compiling, no-CMake build never invokes at all). Not
 * a patch to any upstream file -- config.h has never been part of the
 * source distribution, only ever a build product. */
#ifndef CHOCOLATE_DOOM_CONFIG_H
#define CHOCOLATE_DOOM_CONFIG_H

#define PACKAGE_NAME "chocolate-doom"
#define PACKAGE_TARNAME "chocolate-doom"
#define PACKAGE_VERSION "3.1.1"
#define PACKAGE_STRING "chocolate-doom 3.1.1"
#define PROGRAM_PREFIX "chocolate-"

/* No SDL2_mixer/SDL2_net vendored (no dynamic linker at all to load them
 * against regardless -- see third_party/SDL2/README.md's
 * SDL_LOADSO_DISABLED reasoning). Real, upstream-supported configuration
 * -- see this directory's own README.md. */
#define DISABLE_SDL2MIXER 1
#define DISABLE_SDL2NET 1

/* No FluidSynth, no libsamplerate, no libpng on PureUNIX. */
#undef HAVE_FLUIDSYNTH
#undef HAVE_LIBSAMPLERATE
#undef HAVE_LIBPNG

/* PureUNIX has a real opendir()/readdir() (used by BusyBox ls, TCC's own
 * directory scans, etc.) -- see user/newlib_syscalls.c. */
#define HAVE_DIRENT_H 1

/* Deliberately NOT defined -- see this directory's README.md's "Why no
 * HAVE_MMAP": W_OpenFile() only uses the mmap()-backed WAD reader if the
 * user explicitly passes -mmap, defaulting to the plain fopen()/fread()
 * path either way. PureUNIX's own mmap() only supports anonymous scratch
 * allocations (user/newlib_compat/sys/mman.h), not file-backed mappings,
 * so this could never have worked regardless. */
#undef HAVE_MMAP

/* newlib's <strings.h> really declares strcasecmp()/strncasecmp() (see
 * third_party/htop's own _GNU_SOURCE/HAVE_DECL_* discoveries) -- these
 * gate m_misc.c's own extern declarations, which would otherwise conflict
 * with newlib's. */
#define HAVE_DECL_STRCASECMP 1
#define HAVE_DECL_STRNCASECMP 1

#endif
