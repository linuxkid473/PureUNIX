#ifndef PUREUNIX_VGA_H
#define PUREUNIX_VGA_H

#include <pureunix/types.h>

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15,
};

/* Opaque outside drivers/vga.c: one independent text-mode/framebuffer
 * console (cell grid, cursor, ANSI parser state, scroll region, scrollback
 * ring). kernel/vt.c owns the policy of which index means which VT; this
 * driver only knows "render this one" / "this one owns real hardware". */
typedef struct console console_t;

/* Initializes console 0 (the "boot console" — the only one that exists,
 * and the only one bound to hardware, until kernel/vt.c's vt_init() claims
 * the rest via vga_console()). Must run before any other vga_* call. */
void vga_init(void);

/* console_t pool accessor, n in [0, VGA_MAX_CONSOLES). kernel/vt.c uses
 * indices [0, VT_COUNT) as VT1..VT_COUNT; index 0 is vga_init()'s boot
 * console, already holding real boot-time output, which is why VT1 claims
 * it directly instead of a freshly reset one. Returns NULL out of range. */
console_t *vga_console(int n);
console_t *vga_active_console(void);
bool vga_console_is_active(const console_t *cs);
uint8_t vga_default_color(void);
/* Seeds a brand-new console with a starting color before its first
 * vga_console_reset() -- see drivers/vga.c's comment; without this, a
 * console pulled fresh from the zero-initialized pool renders invisible
 * black-on-black text instead of appearing blank. */
void vga_console_set_color(console_t *cs, uint8_t color);

/* Full reset of one console's buffer/cursor/parser/scroll-region state
 * (repaints real hardware too, if cs happens to already be active) — used
 * to bring a freshly claimed VT2..VT_COUNT console to a clean starting
 * state, matching what vga_init() does for console 0. */
void vga_console_reset(console_t *cs);
/* Full repaint of every cell of cs from its own buffer. No-op unless cs is
 * already the active console (see vga_bind_active()). */
void vga_console_repaint(console_t *cs);
/* Switches which console owns real hardware (framebuffer/text-mode memory,
 * hardware cursor, serial mirroring) and repaints it in full — the
 * rendering half of a VT switch; see kernel/vt.c's vt_switch() for the
 * policy half (keyboard routing, termios, ...). */
void vga_bind_active(console_t *cs);

/* SDL2 platform support (docs/sdl-port.md, kernel/vt.c's
 * vt_set_graphics_mode()) -- suppresses every hardware-paint gate above
 * for cs even while it's the active console, so an SDL app's own
 * framebuffer writes (SYS_FB_BLIT) aren't overwritten by console repaints.
 * Does not itself repaint on disable -- call vga_console_repaint(cs)
 * afterward if cs is still active and should show its console content
 * again. */
void vga_console_set_graphics_mode(console_t *cs, bool enable);
bool vga_console_is_graphics_mode(const console_t *cs);

/* Per-console output — updates cs's own buffer, only touches real hardware
 * if cs is the active console. Used by kernel/vt.c to route a background
 * VT's writes without disturbing the foreground screen. */
void vga_putc_vt(console_t *cs, char c);
void vga_write_len_vt(console_t *cs, const char *str, size_t len);

/* Scrollback: `count` lines of history are available above the live grid
 * (evicted by normal, full-screen scrolling only — see drivers/vga.c).
 * vga_console_scroll_view() shifts the active console's *display* further
 * into history (positive delta) or back toward live (negative), clamped;
 * any new output on a scrolled-back console snaps it back to live before
 * drawing. No-op unless cs is the active console. */
void vga_console_scroll_view(console_t *cs, int delta);
size_t vga_console_scrollback_count(const console_t *cs);

/* ---- Legacy/global API: always targets the currently active console.
 * Used by early boot output, panic(), bootsplash, and the in-kernel
 * recovery shell (shell/sh.c's shell_run() and friends) — none of which
 * run "as" a particular VT's task, so "whatever's on screen right now" is
 * the only sensible target for them. Everything routed through a real
 * per-task VT (SYS_WRITE, drivers/tty.c) uses the _vt variants above via
 * kernel/vt.c instead. ---- */
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *str);
void vga_write_len(const char *str, size_t len);
void vga_set_color(uint8_t fg, uint8_t bg);
uint8_t vga_color(void);
void vga_get_cursor(size_t *row, size_t *col);
/* The console's current text-grid dimensions (see SYS_IOCTL's TIOCGWINSZ in
 * arch/i386/syscall.c) — the same physical grid size for every VT. */
void vga_get_size(size_t *rows, size_t *cols);
void vga_goto(size_t row, size_t col);
void vga_erase_eol(void);
void vga_set_cursor(size_t row, size_t col);
void vga_move_cursor_rel(int drow, int dcol);
void vga_status_bar(const char *left, const char *right);

/* Re-sizes the (hardware-wide) console grid for a new drivers/font.c scale
 * factor and repaints the active console. No-op (returns false) in legacy
 * 80x25 VGA text mode or for an out-of-range scale. See the `font` shell
 * command (user/font.c). */
bool vga_apply_font_scale(int scale);

#endif
