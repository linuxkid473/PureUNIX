#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/keyboard.h>
#include <pureunix/task.h>
#include <pureunix/vt.h>

#define KBD_DATA 0x60
#define KBD_STATUS 0x64

static bool shift_down;
static bool ctrl_down;
static bool alt_down;
static bool caps_lock;
static bool extended;

static const char normal_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = KEY_ENTER,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' ', [0x01] = KEY_ESCAPE,
};

static const char shift_map[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = KEY_ENTER,
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>',
    [0x35] = '?', [0x39] = ' ', [0x01] = KEY_ESCAPE,
};

/* Set-1 function-key scancodes (never 0xE0-prefixed) — F1..F6 are already
 * claimed for Alt+F<n> VT switching above, so this only ever fires for a
 * bare (no Alt) function key. */
static int function_key(uint8_t scancode)
{
    switch (scancode) {
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    default: return KEY_NONE;
    }
}

static int extended_key(uint8_t scancode)
{
    switch (scancode) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PAGE_UP;
    case 0x51: return KEY_PAGE_DOWN;
    case 0x53: return KEY_DELETE;
    default: return KEY_NONE;
    }
}

static void keyboard_irq(interrupt_regs_t *regs)
{
    uint8_t sc = inb(KBD_DATA);
    if (sc == 0xE0) {
        extended = true;
        return;
    }

    bool released = sc & 0x80;
    sc &= 0x7F;

    if (extended) {
        int key = extended_key(sc);
        if (key != KEY_NONE) {
            vt_raw_input_push_key(key, !released);
        }
        if (!released) {
            if (shift_down && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
                /* Shift+PageUp/PageDown browses the active VT's scrollback
                 * (see drivers/vga.c's console_t.sb_view) rather than
                 * producing a normal key event. */
                vt_scroll_view(vt_active_id(), key == KEY_PAGE_UP ? 10 : -10);
            } else {
                vt_input_push(key);
            }
        }
        extended = false;
        return;
    }

    if (sc == 0x2A) {
        shift_down = !released;
        vt_raw_input_push_key(KEY_LSHIFT, !released);
        return;
    }
    if (sc == 0x36) {
        shift_down = !released;
        vt_raw_input_push_key(KEY_RSHIFT, !released);
        return;
    }
    if (sc == 0x1D) {
        ctrl_down = !released;
        vt_raw_input_push_key(KEY_LCTRL, !released);
        return;
    }
    if (sc == 0x38) {
        alt_down = !released;
        vt_raw_input_push_key(KEY_LALT, !released);
        return;
    }
    /* Raw press/release for every plain (non-modifier, non-extended) key,
     * identified by its unshifted mapping regardless of the actual shift
     * state -- see vt.h's vt_raw_input_push_key() comment on why "which key"
     * and "what character" are deliberately kept distinct. Function keys
     * and Caps Lock get their own raw events further below/above; this
     * covers the plain alphanumeric/punctuation block. */
    {
        int rawcode = (uint8_t)normal_map[sc] ? (int)(uint8_t)normal_map[sc] : function_key(sc);
        if (rawcode != KEY_NONE) {
            vt_raw_input_push_key(rawcode, !released);
        }
    }
    if (released) {
        return;
    }
    if (sc == 0x3A) {
        caps_lock = !caps_lock;
        return;
    }
    /* F1..F6 (Set 1 scancodes 0x3B..0x40, never 0xE0-prefixed) + Alt: the
     * one kernel VT-switching path (include/pureunix/vt.h) -- consumed
     * here entirely, never forwarded as a key event. A firmware that routes
     * Fn+Alt+F<n> down to the same Alt+F<n> scancode pair (true of every
     * PS/2-compatible keyboard controller: Fn is handled below the OS,
     * there is no separate Fn scancode) needs nothing extra here. */
    if (alt_down && sc >= 0x3B && sc <= 0x40) {
        vt_switch((int)(sc - 0x3B));
        return;
    }

    int fkey = function_key(sc);
    if (fkey != KEY_NONE) {
        vt_input_push(fkey);
        return;
    }

    int ch = shift_down ? shift_map[sc] : normal_map[sc];
    if (ch >= 'a' && ch <= 'z' && caps_lock) {
        ch = (char)(ch - ('a' - 'A'));
    } else if (ch >= 'A' && ch <= 'Z' && caps_lock && !shift_down) {
        ch = (char)(ch + ('a' - 'A'));
    }

    if (ctrl_down) {
        if (ch == 's' || ch == 'S') ch = KEY_CTRL_S;
        else if (ch == 'q' || ch == 'Q') ch = KEY_CTRL_Q;
        else if (ch == 'f' || ch == 'F') ch = KEY_CTRL_F;
        else if (ch == 'c' || ch == 'C') ch = KEY_CTRL_C;
        else if (ch == 'z' || ch == 'Z') ch = KEY_CTRL_Z;
        else if (ch == '\\') ch = KEY_CTRL_BACKSLASH;
    }
    if (ch) {
        vt_input_push(ch);
    }
}

void keyboard_init(void)
{
    while (inb(KBD_STATUS) & 1) {
        (void)inb(KBD_DATA);
    }
    interrupt_register_handler(33, keyboard_irq);
    irq_enable(1);
}

/* keyboard_getkey()/keyboard_try_getkey() are kept as the public API for
 * backward compatibility with every existing caller (the legacy in-kernel
 * shell/editor, kernel/users.c's login prompt) but now forward to the
 * *calling task's own* VT input queue (include/pureunix/vt.h) -- which is
 * also fed by drivers/hid.c's USB keyboard driver, so callers here see both
 * keyboard types identically, and a call from a backgrounded VT's task
 * simply never sees a key (see vt_input_push()'s "active VT only" delivery
 * rule) instead of stealing one meant for whichever VT is actually on
 * screen. Callers with no VT of their own (task_current()->vt_id == -1 --
 * true only before kernel_main() calls vt_init(), see kernel/task.c) fall
 * back to VT1 (index 0), the boot console. */
static int caller_vt_id(void)
{
    task_t *t = task_current();
    int vt_id = t ? t->vt_id : -1;
    return vt_id >= 0 ? vt_id : 0;
}

int keyboard_try_getkey(void)
{
    return vt_input_try_getkey(caller_vt_id());
}

int keyboard_getkey(void)
{
    return vt_input_getkey(caller_vt_id());
}

bool keyboard_ctrl_down(void)
{
    return ctrl_down;
}
