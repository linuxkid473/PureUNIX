#include <pureunix/arch.h>
#include <pureunix/input.h>
#include <pureunix/io.h>
#include <pureunix/keyboard.h>

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
        if (!released) {
            input_push_key(extended_key(sc));
        }
        extended = false;
        return;
    }

    if (sc == 0x2A || sc == 0x36) {
        shift_down = !released;
        return;
    }
    if (sc == 0x1D) {
        ctrl_down = !released;
        return;
    }
    if (sc == 0x38) {
        alt_down = !released;
        return;
    }
    if (released) {
        return;
    }
    if (sc == 0x3A) {
        caps_lock = !caps_lock;
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
    }
    if (ch) {
        input_push_key(ch);
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
 * backward compatibility with every existing caller (drivers/tty.c, the
 * built-in shell, etc.) but now just forward to the shared input.c queue --
 * see include/pureunix/input.h -- which is also fed by drivers/hid.c's USB
 * keyboard driver, so callers here see both keyboard types identically. */
int keyboard_try_getkey(void)
{
    return input_try_getkey();
}

int keyboard_getkey(void)
{
    return input_getkey();
}

bool keyboard_ctrl_down(void)
{
    return ctrl_down;
}
