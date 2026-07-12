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

/* PureUNIX platform video driver (docs/sdl-port.md): a real vendored
 * framebuffer/input/timing backend, not a stub -- see that doc for the
 * full design. There is no window manager and exactly one physical
 * display, so this is architecturally close to a "software fbdev" SDL
 * port: SDL_CreateWindow() always gets the one true screen resolution
 * (PUREUNIX_CreateWindow forces it), the window's framebuffer is a plain
 * malloc'd pixel buffer already packed in the hardware's own native
 * format (struct pureunix_fb_info -- see SDL_puvideo.h), and
 * UpdateWindowFramebuffer() is a single SYS_FB_BLIT syscall. */

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_puvideo.h"
#include "SDL_puevents_c.h"
#include "SDL_pufb_c.h"
#include "SDL_hints.h"

#define PUREUNIXVID_DRIVER_NAME "pureunix"

static int PUREUNIX_VideoInit(_THIS);
static void PUREUNIX_VideoQuit(_THIS);
static int PUREUNIX_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static int PUREUNIX_CreateWindow(_THIS, SDL_Window *window);
static void PUREUNIX_DestroyWindow(_THIS, SDL_Window *window);

/* Cached once at VideoInit -- struct pureunix_fb_info never changes after
 * boot (one physical display, no hotplug), so there's no reason to repeat
 * the SYS_FB_GETINFO syscall on every window creation. */
static pu_fb_info_t pu_fb;
static Uint32 pu_pixel_format;

static SDL_VideoDevice *PUREUNIX_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (device == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }

    device->VideoInit = PUREUNIX_VideoInit;
    device->VideoQuit = PUREUNIX_VideoQuit;
    device->SetDisplayMode = PUREUNIX_SetDisplayMode;
    device->PumpEvents = PUREUNIX_PumpEvents;

    device->CreateSDLWindow = PUREUNIX_CreateWindow;
    device->DestroyWindow = PUREUNIX_DestroyWindow;

    device->CreateWindowFramebuffer = PUREUNIX_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = PUREUNIX_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = PUREUNIX_DestroyWindowFramebuffer;

    device->free = (void (*)(SDL_VideoDevice *))SDL_free;

    return device;
}

VideoBootStrap PUREUNIX_bootstrap = {
    PUREUNIXVID_DRIVER_NAME, "SDL PureUNIX framebuffer video driver",
    PUREUNIX_CreateDevice
};

static int PUREUNIX_VideoInit(_THIS)
{
    SDL_DisplayMode mode;

    if (pu_fb_getinfo(&pu_fb) != 0) {
        return SDL_SetError("pureunix: no framebuffer available (SYS_FB_GETINFO failed)");
    }

    /* Rmask/Gmask/Bmask are derived straight from the hardware's own field
     * positions (multiboot2's VBE/GOP-reported layout, drivers/
     * framebuffer.c) -- SYS_FB_BLIT expects the caller's buffer already
     * packed exactly this way, so whatever SDL_PixelFormatEnum this
     * resolves to is also exactly what every window's framebuffer will
     * really be, with no per-pixel conversion on the hot path
     * (PUREUNIX_UpdateWindowFramebuffer). */
    Uint32 rmask = ((1u << pu_fb.red_size) - 1u) << pu_fb.red_pos;
    Uint32 gmask = ((1u << pu_fb.green_size) - 1u) << pu_fb.green_pos;
    Uint32 bmask = ((1u << pu_fb.blue_size) - 1u) << pu_fb.blue_pos;
    pu_pixel_format = SDL_MasksToPixelFormatEnum((int)(pu_fb.bypp * 8), rmask, gmask, bmask, 0);
    if (pu_pixel_format == SDL_PIXELFORMAT_UNKNOWN) {
        /* Overwhelmingly unlikely (every VBE/GOP linear framebuffer this
         * kernel has ever been tested against -- QEMU and the HP Pavilion
         * both -- reports plain 32bpp RGB), but fall back to a real format
         * rather than fail outright; SYS_FB_BLIT still expects the
         * hardware's real layout regardless of what SDL calls it, so this
         * only affects how SDL *labels* the format for conversions, not
         * the bytes actually written. */
        pu_pixel_format = pu_fb.bypp == 4 ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_RGB24;
    }

    SDL_zero(mode);
    mode.format = pu_pixel_format;
    mode.w = (int)pu_fb.width;
    mode.h = (int)pu_fb.height;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);

    return 0;
}

static void PUREUNIX_VideoQuit(_THIS)
{
}

Uint32 PUREUNIX_GetPixelFormat(void)
{
    return pu_pixel_format;
}

unsigned int PUREUNIX_GetBytesPerPixel(void)
{
    return pu_fb.bypp;
}

static int PUREUNIX_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    /* One display, one mode (the real hardware resolution) -- nothing to
     * actually switch. */
    return 0;
}

static int PUREUNIX_CreateWindow(_THIS, SDL_Window *window)
{
    /* Forces every window to the one real screen resolution, fullscreen,
     * at the origin -- there is no windowing system to place/resize a
     * window within, so "the window" and "the physical display" are the
     * same thing, the same simplification KMSDRM/RPI/offscreen-style
     * single-display SDL ports already make. */
    window->x = 0;
    window->y = 0;
    window->w = (int)pu_fb.width;
    window->h = (int)pu_fb.height;
    window->flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;

    /* SYS_SET_GRAPHICS_MODE(1): suspend this VT's console text repaint
     * (kernel/vt.c's vt_set_graphics_mode()) so this window's own
     * SYS_FB_BLIT calls own the screen from here until the window is
     * destroyed -- see PUREUNIX_DestroyWindow's matching disable, which is
     * what actually satisfies "SDL applications exit cleanly and restore
     * the terminal" (docs/sdl-port.md). */
    pu_set_graphics_mode(1);

    PUREUNIX_SetEventWindow(window);
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    return 0;
}

static void PUREUNIX_DestroyWindow(_THIS, SDL_Window *window)
{
    PUREUNIX_SetEventWindow(NULL);
    /* Leaving graphics mode forces kernel/vt.c to repaint this VT's real
     * console content immediately if it's still the active VT -- the
     * shell that launched this program reappears exactly as if the
     * program had never run, with no leftover framebuffer garbage on
     * screen. */
    pu_set_graphics_mode(0);
}

#endif /* SDL_VIDEO_DRIVER_PUREUNIX */

/* vi: set ts=4 sw=4 expandtab: */
