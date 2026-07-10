#include <pureunix/hid.h>
#include <pureunix/keyboard.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vt.h>

/* One boot report is always exactly 8 bytes (USB HID spec Appendix B.1):
 * byte0 = modifier bitmap, byte1 = reserved, bytes2-7 = up to six
 * simultaneously-pressed key usage IDs (0 = no key in this slot, 1 =
 * rollover error -- both ignored). */
#define HID_BOOT_REPORT_SIZE 8U

typedef struct hid_keyboard {
    bool in_use;
    uint32_t slot_id;
    uint8_t endpoint_address;
    bool caps_lock;
    uint8_t report_buf[HID_BOOT_REPORT_SIZE]; /* re-armed DMA target, see hid_report_callback() */
    uint8_t prev_report[HID_BOOT_REPORT_SIZE];
} hid_keyboard_t;

static hid_keyboard_t keyboards[HID_MAX_KEYBOARDS];

static hid_keyboard_t *alloc_keyboard(void)
{
    for (uint32_t i = 0; i < HID_MAX_KEYBOARDS; ++i) {
        if (!keyboards[i].in_use) {
            return &keyboards[i];
        }
    }
    return NULL;
}

/* USB HID Usage Tables, Keyboard/Keypad Page (0x07) -- unshifted and
 * shifted printable-character mapping for usage IDs 0x04-0x38, the
 * standard alphanumeric/punctuation block every keyboard layout shares
 * (this driver, like the PS/2 one in drivers/keyboard.c, only supports a
 * US layout). Index 0 (usage ID 0x00) is never a real key and is left as
 * KEY_NONE; entries not populated below (function keys, keypad, etc.) are
 * likewise KEY_NONE -- unsupported rather than misdecoded. */
static const int usage_to_ascii[0x39] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = KEY_ENTER, [0x29] = KEY_ESCAPE, [0x2A] = KEY_BACKSPACE, [0x2B] = KEY_TAB,
    [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const int usage_to_ascii_shift[0x39] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = KEY_ENTER, [0x29] = KEY_ESCAPE, [0x2A] = KEY_BACKSPACE, [0x2B] = KEY_TAB,
    [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

/* Non-printable usage IDs mapped straight to keyboard.h's KEY_* constants
 * -- the same set drivers/keyboard.c's PS/2 driver already produces for its
 * extended (0xE0-prefixed) scancodes, so both keyboards feed
 * indistinguishable events into input.c's shared queue. */
static int extended_usage_key(uint8_t usage)
{
    switch (usage) {
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PAGE_UP;
    case 0x4C: return KEY_DELETE;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PAGE_DOWN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    default: return KEY_NONE;
    }
}

#define HID_USAGE_CAPS_LOCK 0x39U

/* Translates one newly-pressed usage ID into a key event, applying Shift
 * (case/symbol selection), Caps Lock (letters only, matching
 * drivers/keyboard.c's PS/2 behavior exactly), and Ctrl (remapping to the
 * existing KEY_CTRL_* combos the shell/editor already handle) the same way
 * the PS/2 driver's keyboard_irq() does. Toggles *caps_lock itself when the
 * Caps Lock key is the one newly pressed (and returns KEY_NONE, since Caps
 * Lock itself produces no character). */
static int translate_usage(uint8_t usage, bool shift, bool ctrl, bool *caps_lock)
{
    if (usage == HID_USAGE_CAPS_LOCK) {
        *caps_lock = !*caps_lock;
        return KEY_NONE;
    }
    if (usage >= 0x4A && usage <= 0x52) {
        return extended_usage_key(usage);
    }
    if (usage >= sizeof(usage_to_ascii) / sizeof(usage_to_ascii[0])) {
        return KEY_NONE;
    }

    int ch = shift ? usage_to_ascii_shift[usage] : usage_to_ascii[usage];
    if (ch >= 'a' && ch <= 'z' && *caps_lock) {
        ch -= 'a' - 'A';
    } else if (ch >= 'A' && ch <= 'Z' && *caps_lock && !shift) {
        ch += 'a' - 'A';
    }

    if (ctrl) {
        if (ch == 's' || ch == 'S') return KEY_CTRL_S;
        if (ch == 'q' || ch == 'Q') return KEY_CTRL_Q;
        if (ch == 'f' || ch == 'F') return KEY_CTRL_F;
        if (ch == 'c' || ch == 'C') return KEY_CTRL_C;
        if (ch == 'z' || ch == 'Z') return KEY_CTRL_Z;
        if (ch == '\\') return KEY_CTRL_BACKSLASH;
    }
    return ch;
}

static bool usage_in_report(const uint8_t report[HID_BOOT_REPORT_SIZE], uint8_t usage)
{
    for (uint32_t i = 2; i < HID_BOOT_REPORT_SIZE; ++i) {
        if (report[i] == usage) {
            return true;
        }
    }
    return false;
}

/* Diffs the just-received report against the previous one and pushes a key
 * event for every usage ID that's newly present (i.e. actually a fresh
 * key-down, not a still-held key repeated in every ~8-10ms report) --
 * matching the PS/2 driver's press-only event model (key repeat is a later
 * milestone, not yet implemented for either keyboard type). Only presses
 * generate events; releases exist solely to update modifier/Caps Lock
 * state, exactly like keyboard_irq(). */
/* USB HID Usage Tables, Keyboard/Keypad Page (0x07): F1..F6 are usage IDs
 * 0x3A..0x3F -- the same Alt+F<n> VT-switching convention as the PS/2
 * driver's scancode range (drivers/keyboard.c), see include/pureunix/vt.h. */
#define HID_USAGE_F1 0x3A
#define HID_USAGE_F6 0x3F

static void decode_boot_report(hid_keyboard_t *kb)
{
    uint8_t modifiers = kb->report_buf[0];
    bool shift = (modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    bool ctrl = (modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0;
    bool alt = (modifiers & (HID_MOD_LEFT_ALT | HID_MOD_RIGHT_ALT)) != 0;

    for (uint32_t i = 2; i < HID_BOOT_REPORT_SIZE; ++i) {
        uint8_t usage = kb->report_buf[i];
        if (usage == 0 || usage == 1 /* no key / rollover error */) {
            continue;
        }
        if (usage_in_report(kb->prev_report, usage)) {
            continue; /* already held, not a new press */
        }
        if (alt && usage >= HID_USAGE_F1 && usage <= HID_USAGE_F6) {
            vt_switch((int)(usage - HID_USAGE_F1));
            continue;
        }
        int key = translate_usage(usage, shift, ctrl, &kb->caps_lock);
        if (key == KEY_NONE) {
            continue;
        }
        if (shift && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
            vt_scroll_view(vt_active_id(), key == KEY_PAGE_UP ? 10 : -10);
            continue;
        }
        vt_input_push(key);
    }

    memcpy(kb->prev_report, kb->report_buf, HID_BOOT_REPORT_SIZE);
}

/* usb_interrupt_callback_t -- runs inside the host controller's interrupt
 * path (xhci_irq() -> process_event_ring(), see xhci.c) every time a boot
 * report arrives. Must not block: decode_boot_report()/input_push_key()
 * are both simple, bounded, non-blocking operations, matching this
 * kernel's IRQ-context safety rules (see docs/networking.md's deadlock
 * case study). A failed/short transfer is skipped entirely -- the next
 * report (re-armed automatically by the host controller regardless of this
 * callback's outcome) gets another chance rather than treating one bad
 * poll as fatal. */
static void hid_report_callback(uint32_t slot_id, uint8_t endpoint_address, const void *buf,
                                 uint16_t length, bool success, void *ctx)
{
    hid_keyboard_t *kb = (hid_keyboard_t *)ctx;
    if (!success || length < HID_BOOT_REPORT_SIZE) {
        usb_debugf("hid: slot %u: ep %02x: report callback fired but not usable "
                   "(success=%u length=%u)\n",
                   slot_id, endpoint_address, success, length);
        return;
    }
    const uint8_t *bytes = (const uint8_t *)buf;
    usb_debugf("hid: slot %u: ep %02x: report received: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               slot_id, endpoint_address, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
               bytes[5], bytes[6], bytes[7]);
    memcpy(kb->report_buf, buf, HID_BOOT_REPORT_SIZE);
    decode_boot_report(kb);
}

bool hid_try_attach(const usb_hc_ops_t *hc, const usb_device_t *dev)
{
    if (!dev->has_interrupt_in_endpoint) {
        return false;
    }
    if (dev->interface_class != HID_CLASS || dev->interface_subclass != HID_SUBCLASS_BOOT
        || dev->interface_protocol != HID_PROTOCOL_KEYBOARD) {
        return false;
    }

    hid_keyboard_t *kb = alloc_keyboard();
    if (!kb) {
        printf("hid: slot %u: too many Boot Protocol keyboards already attached (max %u)\n",
               dev->slot_id, HID_MAX_KEYBOARDS);
        return false;
    }
    memset(kb, 0, sizeof(*kb));
    kb->slot_id = dev->slot_id;
    kb->endpoint_address = dev->endpoint_address;

    if (!hc->control_transfer(dev->slot_id,
                               USB_REQUEST_TYPE_HOST_TO_DEVICE | USB_REQUEST_TYPE_CLASS
                                   | USB_REQUEST_TYPE_RECIPIENT_INTERFACE,
                               HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, dev->interface_number, 0,
                               NULL, false)) {
        printf("hid: slot %u: SET_PROTOCOL(Boot) failed\n", dev->slot_id);
        return false;
    }
    printf("hid: slot %u: Boot Protocol keyboard attached (interface %u, endpoint %02x)\n",
           dev->slot_id, dev->interface_number, dev->endpoint_address);

    kb->in_use = true;
    if (!hc->submit_interrupt_transfer(dev->slot_id, dev->endpoint_address, kb->report_buf,
                                        sizeof(kb->report_buf), hid_report_callback, kb)) {
        printf("hid: slot %u: failed to submit initial interrupt transfer\n", dev->slot_id);
        kb->in_use = false;
        return false;
    }
    return true;
}
