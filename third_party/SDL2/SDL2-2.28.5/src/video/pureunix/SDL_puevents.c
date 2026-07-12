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

#include "SDL.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#include "SDL_puvideo.h"
#include "SDL_puevents_c.h"

/* There is exactly one physical display and this backend never creates
 * more than one window (see SDL_puvideo.c's PUREUNIX_CreateWindow) -- so,
 * unlike a real multi-window backend, a single static pointer is enough to
 * target every event this pump generates, rather than needing to search a
 * window list by id. */
static SDL_Window *pu_event_window;

void PUREUNIX_SetEventWindow(SDL_Window *window)
{
    pu_event_window = window;
}

/* Maps a PU_INPUT_KEY event's raw, shift-independent code (see
 * include/pureunix/input.h's vt_raw_input_push_key() comment, mirrored as
 * user/pureunix_gfx.h's PU_KEY_* constants) to the SDL_Scancode identifying
 * the same physical key. Returns SDL_SCANCODE_UNKNOWN for anything not
 * mapped below (there is no numpad, no PrintScreen, etc. on the keyboards
 * this port's drivers/keyboard.c and drivers/hid.c actually decode). */
static SDL_Scancode pu_code_to_scancode(int code)
{
    switch (code) {
    case 'a': return SDL_SCANCODE_A;
    case 'b': return SDL_SCANCODE_B;
    case 'c': return SDL_SCANCODE_C;
    case 'd': return SDL_SCANCODE_D;
    case 'e': return SDL_SCANCODE_E;
    case 'f': return SDL_SCANCODE_F;
    case 'g': return SDL_SCANCODE_G;
    case 'h': return SDL_SCANCODE_H;
    case 'i': return SDL_SCANCODE_I;
    case 'j': return SDL_SCANCODE_J;
    case 'k': return SDL_SCANCODE_K;
    case 'l': return SDL_SCANCODE_L;
    case 'm': return SDL_SCANCODE_M;
    case 'n': return SDL_SCANCODE_N;
    case 'o': return SDL_SCANCODE_O;
    case 'p': return SDL_SCANCODE_P;
    case 'q': return SDL_SCANCODE_Q;
    case 'r': return SDL_SCANCODE_R;
    case 's': return SDL_SCANCODE_S;
    case 't': return SDL_SCANCODE_T;
    case 'u': return SDL_SCANCODE_U;
    case 'v': return SDL_SCANCODE_V;
    case 'w': return SDL_SCANCODE_W;
    case 'x': return SDL_SCANCODE_X;
    case 'y': return SDL_SCANCODE_Y;
    case 'z': return SDL_SCANCODE_Z;
    case '1': return SDL_SCANCODE_1;
    case '2': return SDL_SCANCODE_2;
    case '3': return SDL_SCANCODE_3;
    case '4': return SDL_SCANCODE_4;
    case '5': return SDL_SCANCODE_5;
    case '6': return SDL_SCANCODE_6;
    case '7': return SDL_SCANCODE_7;
    case '8': return SDL_SCANCODE_8;
    case '9': return SDL_SCANCODE_9;
    case '0': return SDL_SCANCODE_0;
    case '-': return SDL_SCANCODE_MINUS;
    case '=': return SDL_SCANCODE_EQUALS;
    case '[': return SDL_SCANCODE_LEFTBRACKET;
    case ']': return SDL_SCANCODE_RIGHTBRACKET;
    case '\\': return SDL_SCANCODE_BACKSLASH;
    case ';': return SDL_SCANCODE_SEMICOLON;
    case '\'': return SDL_SCANCODE_APOSTROPHE;
    case '`': return SDL_SCANCODE_GRAVE;
    case ',': return SDL_SCANCODE_COMMA;
    case '.': return SDL_SCANCODE_PERIOD;
    case '/': return SDL_SCANCODE_SLASH;
    case ' ': return SDL_SCANCODE_SPACE;
    case PU_KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case PU_KEY_TAB: return SDL_SCANCODE_TAB;
    case PU_KEY_ENTER: return SDL_SCANCODE_RETURN;
    case PU_KEY_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case PU_KEY_UP: return SDL_SCANCODE_UP;
    case PU_KEY_DOWN: return SDL_SCANCODE_DOWN;
    case PU_KEY_LEFT: return SDL_SCANCODE_LEFT;
    case PU_KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case PU_KEY_HOME: return SDL_SCANCODE_HOME;
    case PU_KEY_END: return SDL_SCANCODE_END;
    case PU_KEY_PAGE_UP: return SDL_SCANCODE_PAGEUP;
    case PU_KEY_PAGE_DOWN: return SDL_SCANCODE_PAGEDOWN;
    case PU_KEY_DELETE: return SDL_SCANCODE_DELETE;
    case PU_KEY_F1: return SDL_SCANCODE_F1;
    case PU_KEY_F2: return SDL_SCANCODE_F2;
    case PU_KEY_F3: return SDL_SCANCODE_F3;
    case PU_KEY_F4: return SDL_SCANCODE_F4;
    case PU_KEY_F5: return SDL_SCANCODE_F5;
    case PU_KEY_F6: return SDL_SCANCODE_F6;
    case PU_KEY_F7: return SDL_SCANCODE_F7;
    case PU_KEY_F8: return SDL_SCANCODE_F8;
    case PU_KEY_F9: return SDL_SCANCODE_F9;
    case PU_KEY_F10: return SDL_SCANCODE_F10;
    case PU_KEY_F11: return SDL_SCANCODE_F11;
    case PU_KEY_F12: return SDL_SCANCODE_F12;
    case PU_KEY_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case PU_KEY_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case PU_KEY_LCTRL: return SDL_SCANCODE_LCTRL;
    case PU_KEY_RCTRL: return SDL_SCANCODE_RCTRL;
    case PU_KEY_LALT: return SDL_SCANCODE_LALT;
    case PU_KEY_RALT: return SDL_SCANCODE_RALT;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

static Uint8 pu_mouse_button_to_sdl(int code)
{
    switch (code) {
    case PU_MOUSE_BTN_LEFT: return SDL_BUTTON_LEFT;
    case PU_MOUSE_BTN_RIGHT: return SDL_BUTTON_RIGHT;
    case PU_MOUSE_BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
    default: return SDL_BUTTON_LEFT;
    }
}

void PUREUNIX_PumpEvents(_THIS)
{
    pu_input_event_t ev;

    if (!pu_event_window) {
        return;
    }

    /* Drains the whole queue every call rather than just one event --
     * pu_input_poll() (SYS_INPUT_POLL) never blocks, so there's no
     * "process one, come back next frame" cost/benefit tradeoff the way
     * there would be for a blocking read; draining fully keeps input
     * latency down to a single PumpEvents() call even under a burst of
     * fast mouse motion (include/pureunix/vt.h's raw queue can hold up to
     * 256 queued events between frames). */
    while (pu_input_poll(&ev) == 1) {
        switch (ev.type) {
        case PU_INPUT_KEY: {
            SDL_Scancode sc = pu_code_to_scancode(ev.code);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(ev.value ? SDL_PRESSED : SDL_RELEASED, sc);
            }
            break;
        }
        case PU_INPUT_MOUSE_MOTION:
            SDL_SendMouseMotion(pu_event_window, 0, 0, ev.x, ev.y);
            break;
        case PU_INPUT_MOUSE_BUTTON:
            SDL_SendMouseButton(pu_event_window, 0, ev.value ? SDL_PRESSED : SDL_RELEASED,
                                 pu_mouse_button_to_sdl(ev.code));
            break;
        default:
            break;
        }
    }
}

#endif /* SDL_VIDEO_DRIVER_PUREUNIX */

/* vi: set ts=4 sw=4 expandtab: */
