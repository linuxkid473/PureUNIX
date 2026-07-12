#ifndef PUREUNIX_GFX_H
#define PUREUNIX_GFX_H

/* Userspace-facing declarations for the SDL2 platform support syscalls
 * (SYS_INPUT_POLL/SYS_FB_GETINFO/SYS_FB_BLIT/SYS_GET_TICKS_MS/
 * SYS_SET_GRAPHICS_MODE -- include/pureunix/syscall.h, docs/sdl-port.md),
 * implemented in user/newlib_syscalls.c. This is the entire interface
 * between src/video/pureunix/ (third_party/SDL2) and the kernel -- kept
 * deliberately narrow and separate from libpure.h/the rest of newlib so
 * the SDL backend's dependency on PureUNIX specifics is easy to see in one
 * place. */

#include <stdint.h>

/* Layout must match struct pureunix_fb_info in
 * include/pureunix/framebuffer.h. */
typedef struct pu_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t bypp;
    uint32_t red_pos, red_size;
    uint32_t green_pos, green_size;
    uint32_t blue_pos, blue_size;
} pu_fb_info_t;

/* PU_INPUT_KEY event codes -- must match include/pureunix/keyboard.h's
 * KEY_* enum values exactly (a plain ASCII byte for the printable/control
 * keys, sequential values from KEY_BASE up for everything else -- these
 * are the current values of that sequence). */
#define PU_KEY_BACKSPACE 8
#define PU_KEY_TAB       9
#define PU_KEY_ENTER     10
#define PU_KEY_ESCAPE    27
#define PU_KEY_BASE      0x100
enum {
    PU_KEY_UP = PU_KEY_BASE,
    PU_KEY_DOWN,
    PU_KEY_LEFT,
    PU_KEY_RIGHT,
    PU_KEY_HOME,
    PU_KEY_END,
    PU_KEY_PAGE_UP,
    PU_KEY_PAGE_DOWN,
    PU_KEY_DELETE,
    PU_KEY_CTRL_S,
    PU_KEY_CTRL_Q,
    PU_KEY_CTRL_F,
    PU_KEY_CTRL_C,
    PU_KEY_CTRL_Z,
    PU_KEY_CTRL_BACKSLASH,
    PU_KEY_F1,
    PU_KEY_F2,
    PU_KEY_F3,
    PU_KEY_F4,
    PU_KEY_F5,
    PU_KEY_F6,
    PU_KEY_F7,
    PU_KEY_F8,
    PU_KEY_F9,
    PU_KEY_F10,
    PU_KEY_F11,
    PU_KEY_F12,
    PU_KEY_LSHIFT,
    PU_KEY_RSHIFT,
    PU_KEY_LCTRL,
    PU_KEY_RCTRL,
    PU_KEY_LALT,
    PU_KEY_RALT,
};

/* Layout must match pu_input_event_t in include/pureunix/input.h. */
enum {
    PU_INPUT_KEY = 1,
    PU_INPUT_MOUSE_MOTION = 2,
    PU_INPUT_MOUSE_BUTTON = 3,
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

/* Returns 1 and fills *out if an event was available, 0 if the queue was
 * empty, or a negative errno. Never blocks. */
int pu_input_poll(pu_input_event_t *out);
/* Returns 0 and fills *out, or a negative errno (-ENODEV: no framebuffer). */
int pu_fb_getinfo(pu_fb_info_t *out);
/* buf must be exactly out->width * out->height * out->bypp bytes, packed
 * per *out's red/green/blue field layout, no row padding. Returns 0 or a
 * negative errno. */
int pu_fb_blit(const void *buf, unsigned int len);
/* Milliseconds since boot (10ms real resolution -- see
 * include/pureunix/syscall.h's SYS_GET_TICKS_MS comment). */
unsigned int pu_get_ticks_ms(void);
/* enable: 1 to enter graphics mode (suspend console repaint of this VT so
 * pu_fb_blit() owns the screen), 0 to leave it (forces a console repaint
 * if still the active VT). */
void pu_set_graphics_mode(int enable);
/* Maps (once per process -- safe to call again, returns the same pointer)
 * a dedicated window-surface pixel buffer sized to the real framebuffer's
 * width*height*bypp, tightly packed. Not backed by malloc()/the newlib
 * heap -- see include/pureunix/syscall.h's SYS_FB_MMAP comment. Returns
 * NULL on failure. */
void *pu_fb_mmap(void);

#endif
