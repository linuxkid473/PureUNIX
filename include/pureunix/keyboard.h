#ifndef PUREUNIX_KEYBOARD_H
#define PUREUNIX_KEYBOARD_H

#include <pureunix/types.h>

enum {
    KEY_NONE = 0,
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_ENTER = 10,
    KEY_ESCAPE = 27,
    KEY_BASE = 0x100,
    /* Explicit "= KEY_BASE" is load-bearing, not decorative: KEY_BASE is
     * itself a real enum member here (unlike user/pureunix_gfx.h's
     * PU_KEY_BASE, a plain #define that never consumes a value), so
     * without this, KEY_UP would silently become KEY_BASE+1 (0x101) and
     * every constant below it would drift one further from its PU_KEY_*
     * counterpart — exactly the bug this once was: Chocolate Doom's
     * Right arrow (KEY_RIGHT, raw-queue-only) silently became
     * SDL_SCANCODE_HOME once it crossed into user/pureunix_gfx.h's
     * independently-numbered PU_KEY_* constants (see SDL_puevents.c),
     * while Left became SDL_SCANCODE_RIGHT, Down became SDL_SCANCODE_LEFT,
     * and Up became SDL_SCANCODE_DOWN. Invisible to every ASCII/escape-
     * sequence consumer (drivers/tty.c's key_to_escape_seq() compares
     * against these same symbolic constants directly, never crossing into
     * PU_KEY_* land), which is why ncurses/vi/htop's arrow keys were never
     * affected and this went unnoticed until Chocolate Doom's raw SDL
     * input path exposed it. */
    KEY_UP = KEY_BASE,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_DELETE,
    KEY_CTRL_S,
    KEY_CTRL_Q,
    KEY_CTRL_F,
    KEY_CTRL_C,
    /* Ctrl+Z (SIGTSTP) / Ctrl+\ (SIGQUIT) — see drivers/tty.c's
     * key_to_byte(), which maps these (and KEY_CTRL_C above) to their
     * real termios control-character byte values (VSUSP/VQUIT/VINTR —
     * kernel/vt.c's termios_defaults()) rather than delivering them as
     * literal keystrokes, and docs/process-management.md. */
    KEY_CTRL_Z,
    KEY_CTRL_BACKSLASH,
    /* Function keys F1..F12 — bare (no Alt), since Alt+F1..F6 is already
     * claimed for VT switching (drivers/keyboard.c). Translated to real
     * ANSI/xterm escape sequences by drivers/tty.c's key_to_seq(), matching
     * docs/ncurses-port.md's "pureunix" terminfo entry's kf1..kf12. */
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    /* Modifier keys as raw events (include/pureunix/input.h) -- these never
     * flow through vt_input_push()'s ASCII queue (modifiers alone produce
     * no character), only through vt_raw_input_push_key(), so SDL can track
     * shift/ctrl/alt state directly instead of inferring it from which
     * shifted character arrived. */
    KEY_LSHIFT,
    KEY_RSHIFT,
    KEY_LCTRL,
    KEY_RCTRL,
    KEY_LALT,
    KEY_RALT,
};

void keyboard_init(void);
int keyboard_getkey(void);
int keyboard_try_getkey(void);
bool keyboard_ctrl_down(void);

#endif
