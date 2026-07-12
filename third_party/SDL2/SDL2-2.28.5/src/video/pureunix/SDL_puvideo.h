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

#ifndef SDL_puvideo_h_
#define SDL_puvideo_h_

#include "../SDL_sysvideo.h"
#include "pureunix_gfx.h"

/* PureUNIX platform video backend (docs/sdl-port.md). There is no window
 * manager and never more than one physical display, so unlike a real
 * multi-window backend this only ever tracks a single SDL_Window -- see
 * PUVID_window's comment in SDL_puvideo.c. */

typedef struct SDL_WindowData
{
    /* Tightly-packed (no row padding), native-format software backing
     * store for this window -- what SDL_CreateWindowFramebuffer() hands
     * back as *pixels, and what UpdateWindowFramebuffer() hands to
     * pu_fb_blit() unchanged (same format the hardware itself uses, see
     * struct pureunix_fb_info -- no per-pixel repacking needed). */
    void *pixels;
    int pitch;
    unsigned int len;
} SDL_WindowData;

/* Set once by PUREUNIX_VideoInit() (SDL_puvideo.c); read by
 * PUREUNIX_CreateWindowFramebuffer() (SDL_pufb.c) to size/label each
 * window's backing store to match the real hardware format exactly. */
extern Uint32 PUREUNIX_GetPixelFormat(void);
extern unsigned int PUREUNIX_GetBytesPerPixel(void);

#endif /* SDL_puvideo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
