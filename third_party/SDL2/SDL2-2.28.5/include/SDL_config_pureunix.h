/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef SDL_config_pureunix_h_
#define SDL_config_pureunix_h_
#define SDL_config_h_

#include "SDL_platform.h"

/**
 *  \file SDL_config_pureunix.h
 *
 *  PureUNIX platform configuration -- see docs/sdl-port.md in the PureUNIX
 *  tree. Modeled on SDL_config_windows.h's HAVE_LIBC block: PureUNIX
 *  userspace links against a real C library (a vendored newlib,
 *  third_party/newlib -- already proven by the TCC/Lua/SQLite/ncurses/htop
 *  ports), so almost everything below maps straight onto real libc calls
 *  rather than SDL's own bundled fallback implementations (under
 *  src/stdlib) the way a truly libc-less target (SDL_config_minimal.h)
 *  would need to.
 *
 *  What's cleanly disabled instead of stubbed, and why (docs/sdl-port.md
 *  has the full rationale): threads (PureUNIX has no shared-address-space
 *  lightweight thread primitive, only fork()-style processes -- adding one
 *  is future work, not this port), the dynamic loader (no dlopen, same
 *  reasoning as third_party/lua's LUA_USE_DLOPEN omission), joystick/
 *  haptic/sensor/HIDAPI (no input devices of those kinds exist yet), and
 *  audio (no usable audio subsystem yet -- see docs/sdl-port.md). Video,
 *  events, and timing are all real (src/video/pureunix/,
 *  src/timer/pureunix/), not stubs.
 */

#define SIZEOF_VOIDP 4

#define HAVE_GCC_ATOMICS 1

#define HAVE_LIBC 1

#define HAVE_ALLOCA_H    1
#define HAVE_CTYPE_H     1
#define HAVE_FLOAT_H     1
#define HAVE_LIMITS_H    1
#define HAVE_MATH_H      1
#define HAVE_SIGNAL_H    1
#define HAVE_STDARG_H    1
#define HAVE_STDDEF_H    1
#define HAVE_STDINT_H    1
#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_SYS_TYPES_H 1

#define HAVE_MALLOC   1
#define HAVE_CALLOC   1
#define HAVE_REALLOC  1
#define HAVE_FREE     1
#define HAVE_ALLOCA   1

#define HAVE_GETENV   1
#define HAVE_SETENV   1

#define HAVE_QSORT    1
#define HAVE_BSEARCH  1
#define HAVE_ABS      1
#define HAVE_MEMSET   1
#define HAVE_MEMCPY   1
#define HAVE_MEMMOVE  1
#define HAVE_MEMCMP   1

#define HAVE_STRLEN     1
#define HAVE_INDEX      1
#define HAVE_RINDEX     1
#define HAVE_STRCHR     1
#define HAVE_STRRCHR    1
#define HAVE_STRSTR     1
#define HAVE_STRTOK_R   1
#define HAVE_STRTOL     1
#define HAVE_STRTOUL    1
#define HAVE_STRTOLL    1
#define HAVE_STRTOULL   1
#define HAVE_STRTOD     1
#define HAVE_ATOI       1
#define HAVE_ATOF       1
#define HAVE_STRCMP     1
#define HAVE_STRNCMP    1
#define HAVE_STRCASECMP   1
#define HAVE_STRNCASECMP  1
#define HAVE_SSCANF     1
#define HAVE_VSSCANF    1
#define HAVE_SNPRINTF   1
#define HAVE_VSNPRINTF  1

#define HAVE_M_PI 1
#define HAVE_ACOS     1
#define HAVE_ACOSF    1
#define HAVE_ASIN     1
#define HAVE_ASINF    1
#define HAVE_ATAN     1
#define HAVE_ATANF    1
#define HAVE_ATAN2    1
#define HAVE_ATAN2F   1
#define HAVE_CEIL     1
#define HAVE_CEILF    1
#define HAVE_COPYSIGN  1
#define HAVE_COPYSIGNF 1
#define HAVE_COS      1
#define HAVE_COSF     1
#define HAVE_EXP      1
#define HAVE_EXPF     1
#define HAVE_FABS     1
#define HAVE_FABSF    1
#define HAVE_FLOOR    1
#define HAVE_FLOORF   1
#define HAVE_FMOD     1
#define HAVE_FMODF    1
#define HAVE_LOG      1
#define HAVE_LOGF     1
#define HAVE_LOG10    1
#define HAVE_LOG10F   1
#define HAVE_POW      1
#define HAVE_POWF     1
#define HAVE_SCALBN   1
#define HAVE_SCALBNF  1
#define HAVE_SIN      1
#define HAVE_SINF     1
#define HAVE_SQRT     1
#define HAVE_SQRTF    1
#define HAVE_TAN      1
#define HAVE_TANF     1
#define HAVE_TRUNC    1
#define HAVE_TRUNCF   1

#define HAVE_SETJMP   1
#define HAVE_NANOSLEEP 1

/* No sub-second-resolution wall clock and no sigaction()-driven realtime
 * signals -- see docs/sdl-port.md. SDL_sigaction() (src/events/SDL_events.c)
 * is only used for its own SIGINT/SIGTERM quit-request hook, which this
 * port deliberately does not enable (no HAVE_SIGACTION), matching a plain
 * SDL_QUIT-less-signal-handling platform; apps still get SDL_QUIT via
 * ordinary window-close/quit-request paths. */
#undef HAVE_SIGACTION
#undef HAVE_CLOCK_GETTIME
#undef HAVE_GETPAGESIZE
#undef HAVE_MPROTECT
#undef HAVE_ICONV
#undef HAVE_DLOPEN

/* No shared-address-space lightweight threads (docs/sdl-port.md) -- the
 * generic "stub thread support" branch (src/thread/generic) makes every
 * SDL_CreateThread() call fail cleanly with SDL_SetError() rather than
 * silently doing nothing, and SDL_LockMutex()/SDL_CondWait()/etc. degrade
 * to plain no-ops -- correct for a single-threaded event/render loop, which
 * is all this port's video/event/timer path ever needs. */
#define SDL_THREADS_DISABLED 1

/* No dynamic linker at all -- src/loadso/dummy's real upstream fallback,
 * same reasoning as third_party/lua's LUA_USE_DLOPEN omission. */
#define SDL_LOADSO_DISABLED 1

/* No joystick/gamepad, haptic, or sensor hardware support yet. */
#define SDL_JOYSTICK_DISABLED 1
#define SDL_HAPTIC_DISABLED   1
#define SDL_SENSOR_DISABLED   1
#define SDL_HIDAPI_DISABLED   1

/* No usable audio subsystem yet (docs/sdl-port.md) -- the dummy driver
 * accepts SDL_OpenAudioDevice() calls and silently discards every buffer,
 * exactly like SDL_AUDIO_DRIVER_DUMMY does on any other platform, so an
 * app that merely calls SDL_Init(SDL_INIT_AUDIO) doesn't fail outright.
 * Architected so a real driver can be added later (see that doc's "Future
 * work" section) without touching anything above. */
#define SDL_AUDIO_DRIVER_DUMMY 1

/* Real framebuffer/input/timing backends -- see src/video/pureunix/ and
 * src/timer/pureunix/, docs/sdl-port.md. */
#define SDL_VIDEO_DRIVER_PUREUNIX 1
#define SDL_TIMER_PUREUNIX 1

/* No windowing system, so no more than one "window" (the whole physical
 * screen) will ever exist -- same reasoning KMSDRM/RPI/offscreen-style
 * single-display ports use for these. */
#define SDL_VIDEO_RENDER_SW 1

#define SDL_FILESYSTEM_DUMMY 1

#endif /* SDL_config_pureunix_h_ */
