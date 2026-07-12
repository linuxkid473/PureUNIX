#ifndef PUREUNIX_INPUT_H
#define PUREUNIX_INPUT_H

#include <pureunix/types.h>

/* Raw input events -- a second, parallel event source alongside the
 * existing ASCII/KEY_* queue (include/pureunix/vt.h's vt_input_push()),
 * added for SDL2 (docs/sdl-port.md): SDL needs real key-down *and* key-up
 * events carrying a stable per-physical-key identity, plus mouse motion/
 * button events, none of which the line-oriented tty model has any reason
 * to produce. Every keyboard driver (drivers/keyboard.c's PS/2 and
 * drivers/hid.c's USB Boot Protocol) and every mouse driver
 * (drivers/mouse.c's PS/2 and drivers/hid.c's USB Boot Protocol) feeds
 * this queue *in addition to*, not instead of, the existing one -- so
 * nothing about tty/shell/ncurses input behavior changes.
 *
 * Delivered only to the currently-active VT's own queue, exactly like
 * vt_input_push() -- see kernel/vt.c. */

enum {
    PU_INPUT_KEY = 1,          /* code: KEY_* constant or ASCII value (see
                                 * include/pureunix/keyboard.h) identifying
                                 * *which* key, independent of shift state.
                                 * value: 1 == pressed, 0 == released. */
    PU_INPUT_MOUSE_MOTION = 2, /* dx/dy: relative motion since the last
                                 * event, already clamped to the screen by
                                 * the kernel (see drivers/mouse.c). x/y:
                                 * the resulting absolute pointer position. */
    PU_INPUT_MOUSE_BUTTON = 3, /* code: PU_MOUSE_BTN_* below. value: 1 ==
                                 * pressed, 0 == released. */
};

enum {
    PU_MOUSE_BTN_LEFT = 0,
    PU_MOUSE_BTN_RIGHT = 1,
    PU_MOUSE_BTN_MIDDLE = 2,
};

typedef struct pu_input_event {
    uint32_t type;
    int32_t code;
    int32_t value;
    int32_t dx, dy;
    int32_t x, y;
} pu_input_event_t;

#endif
