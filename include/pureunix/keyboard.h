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
    KEY_UP,
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
};

void keyboard_init(void);
int keyboard_getkey(void);
int keyboard_try_getkey(void);
bool keyboard_ctrl_down(void);

#endif
