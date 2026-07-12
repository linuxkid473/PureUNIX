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
#include "../../SDL_internal.h"

#ifdef SDL_TIMER_PUREUNIX

/* PureUNIX timer backend (docs/sdl-port.md) -- backed by SYS_GET_TICKS_MS
 * (include/pureunix/syscall.h), itself the PIT tick counter at 100Hz, so
 * every value here has genuine 10ms granularity despite the millisecond
 * unit -- coarse, but real: SDL_GetTicks()/SDL_Delay() correctness (not
 * merely existing) is this port's whole timing requirement, and 10ms is
 * finer than a 35fps Doom-style tic (~28ms), the actual target workload
 * (docs/sdl-port.md's "larger purpose"). Unlike src/timer/unix, there is
 * no clock_gettime()/gettimeofday() with sub-second resolution to fall
 * back to -- see SDL_config_pureunix.h's HAVE_CLOCK_GETTIME omission. */

#include "SDL_timer.h"
#include "../SDL_timer_c.h"

#include "pureunix_gfx.h"
#include <time.h>

static SDL_bool ticks_started = SDL_FALSE;
static Uint32 start_ms;

void SDL_TicksInit(void)
{
    if (ticks_started) {
        return;
    }
    ticks_started = SDL_TRUE;
    start_ms = pu_get_ticks_ms();
}

void SDL_TicksQuit(void)
{
    ticks_started = SDL_FALSE;
}

Uint64 SDL_GetTicks64(void)
{
    if (!ticks_started) {
        SDL_TicksInit();
    }
    /* Wraparound-safe even across the 32-bit pu_get_ticks_ms() rolling
     * over (~49.7 days uptime): unsigned subtraction of two Uint32 values
     * produces the correct forward distance regardless of wraparound,
     * exactly like Linux's jiffies comparisons. */
    return (Uint64)(pu_get_ticks_ms() - start_ms);
}

Uint64 SDL_GetPerformanceCounter(void)
{
    if (!ticks_started) {
        SDL_TicksInit();
    }
    return (Uint64)pu_get_ticks_ms();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000;
}

void SDL_Delay(Uint32 ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

#endif /* SDL_TIMER_PUREUNIX */

/* vi: set ts=4 sw=4 expandtab: */
