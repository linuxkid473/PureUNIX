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

#if SDL_VIDEO_DRIVER_PUREUNIX

#include "../SDL_sysvideo.h"
#include "SDL_puvideo.h"
#include "SDL_pufb_c.h"
#include "pureunix_gfx.h"

int PUREUNIX_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_WindowData *data;
    int w, h;

    PUREUNIX_DestroyWindowFramebuffer(_this, window);

    data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    if (data == NULL) {
        return SDL_OutOfMemory();
    }

    SDL_GetWindowSizeInPixels(window, &w, &h);

    Uint32 fmt = PUREUNIX_GetPixelFormat();
    unsigned int bypp = PUREUNIX_GetBytesPerPixel();

    data->pitch = (int)((unsigned int)w * bypp);
    data->len = (unsigned int)data->pitch * (unsigned int)h;
    /* SYS_FB_MMAP (arch/i386/syscall.c), not SDL_calloc(): a real
     * framebuffer-sized buffer (a few MiB at any common resolution) is far
     * bigger than newlib's small per-process heap array
     * (user/newlib_syscalls.c's NEWLIB_HEAP_SIZE) can spare -- see that
     * constant's own comment for why it's deliberately not sized to fit
     * this instead. The kernel owns this memory for the rest of the
     * process's lifetime; nothing here ever frees it (see
     * PUREUNIX_DestroyWindowFramebuffer below). */
    data->pixels = pu_fb_mmap();
    if (data->pixels == NULL) {
        SDL_free(data);
        return SDL_OutOfMemory();
    }

    window->driverdata = data;
    *format = fmt;
    *pixels = data->pixels;
    *pitch = data->pitch;
    return 0;
}

int PUREUNIX_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (data == NULL || data->pixels == NULL) {
        return SDL_SetError("pureunix: no window framebuffer to update");
    }
    /* Always a full-screen blit regardless of `rects` -- SYS_FB_BLIT
     * (arch/i386/syscall.c) only supports "the whole frame" today, and a
     * dirty-rect partial blit is a pure performance optimization, not a
     * correctness requirement (docs/sdl-port.md); every caller of
     * SDL_UpdateWindowSurface() already expects the whole window's current
     * content to end up visible either way. */
    int r = pu_fb_blit(data->pixels, data->len);
    if (r != 0) {
        return SDL_SetError("pureunix: SYS_FB_BLIT failed (%d)", r);
    }
    return 0;
}

void PUREUNIX_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (data) {
        /* data->pixels is SYS_FB_MMAP's kernel-owned mapping, not an
         * SDL_calloc() allocation -- never SDL_free()'d; it (and every
         * other page in this process's address space) is reclaimed in one
         * shot when the process itself exits (kernel/task.c's address-
         * space teardown), the same way a program's ELF-loaded code/data
         * already is. */
        SDL_free(data);
        window->driverdata = NULL;
    }
}

#endif /* SDL_VIDEO_DRIVER_PUREUNIX */

/* vi: set ts=4 sw=4 expandtab: */
