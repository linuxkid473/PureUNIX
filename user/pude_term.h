#ifndef PUDE_TERM_H
#define PUDE_TERM_H

#include "pude_app.h"

/* PUTerm's own terminal emulator core: a cell grid + real ANSI/VT100-subset
 * escape-sequence interpreter, fed raw bytes read from the pty master
 * (include/pureunix/pty.h) and consumed by user/pude.c's renderer (font
 * glyphs blitted to the SDL2 window surface). This is deliberately its own
 * implementation, not a call into drivers/vga.c (kernel-only, and coupled
 * to real VGA/framebuffer hardware state) — see docs/pude.md for why a
 * terminal emulator owning its own escape-sequence interpretation is the
 * right architecture here, not a shortcut.
 *
 * The exact escape-sequence subset implemented is drivers/vga.c's own
 * (third_party/ncurses/pureunix.terminfo documents it capability-by-
 * capability) — CSI cursor movement/erase/SGR, IND/RI, DECSC/DECRC, and
 * DECSTBM scroll regions — so anything already working against PureUNIX's
 * physical VT console (ash, coreutils, Lua, htop, ncurses apps) produces
 * the same visible result inside PUTerm.
 */
#include <stddef.h>

#define TERM_MAX_ROWS 80
#define TERM_MAX_COLS 220
#define TERM_SCROLLBACK 500

/* A 16-color palette index (matching drivers/vga.c's own SGR color
 * handling) plus bold/reverse — enough for every real program this port
 * targets (ash, coreutils, Lua, htop, ncurses apps all restrict themselves
 * to the same 16-color ANSI set the physical console supports). */
typedef struct {
    char ch;
    unsigned char fg;   /* 0-15, or 0xFF for "default" */
    unsigned char bg;   /* 0-15, or 0xFF for "default" */
    unsigned char bold;
    unsigned char reverse;
} term_cell_t;

typedef struct {
    int rows, cols;
    term_cell_t cell[TERM_MAX_ROWS][TERM_MAX_COLS];

    int cur_row, cur_col;
    int cursor_visible;

    /* Current SGR state applied to newly-written cells. */
    unsigned char cur_fg, cur_bg, cur_bold, cur_reverse;

    /* DECSTBM scroll region (0-based, inclusive), defaults to the whole
     * grid — mirrors drivers/vga.c's scroll_top/scroll_bottom. */
    int scroll_top, scroll_bottom;

    /* DECSC/DECRC (\E7/\E8) saved cursor position. */
    int saved_row, saved_col;

    /* Escape-sequence parser state. */
    enum { TS_NORMAL, TS_ESC, TS_CSI } state;
    char csi_buf[32];
    int csi_len;
    int csi_private; /* '?' seen (civis/cnorm-style private-mode sequences) */

    /* Plain-text scrollback ring (no attributes — same simplification
     * drivers/vga.c's own scrollback makes, see docs/vt.md), pushed one row
     * at a time whenever a line scrolls off the top of the live grid. */
    char sb[TERM_SCROLLBACK][TERM_MAX_COLS];
    int sb_count, sb_next;
    int sb_view; /* 0 = live; >0 = scrolled back this many lines */

    /* Set whenever the grid changes, cleared by the renderer once it has
     * redrawn — lets user/pude.c skip a redraw on frames where nothing
     * happened, exactly like real terminal emulators throttle redraws. */
    int dirty;
} term_t;

void term_init(term_t *t, int rows, int cols);
void term_feed(term_t *t, const char *data, size_t len);
void term_scroll_view(term_t *t, int delta);

/* Live resize (dragging the window's resize grip, user/pude.c) — clamped
 * to [1, TERM_MAX_ROWS/COLS]. Existing content is preserved, anchored at
 * the top-left; newly-exposed cells (growing) are blanked; a shrink simply
 * clips (real terminal emulators don't reflow text on resize either — the
 * redrawing program, e.g. ash/vi's next repaint, is what fixes up the
 * display, exactly like a real xterm resize). Resets the scroll region to
 * the new full screen and clamps the cursor into bounds. A no-op if the
 * size is unchanged. */
void term_resize(term_t *t, int rows, int cols);

/* PUTerm as a pluggable `pude` app (user/pude_app.h) -- registered in
 * user/pude.c's launcher app table. Wraps this file's term_t escape-
 * sequence interpreter together with a real pty (include/pureunix/pty.h)
 * and a forked+exec'd BusyBox ash, exactly as docs/pude.md describes;
 * each window spawned from the launcher gets its own independent instance
 * of all three (its own pty, its own shell process, its own term_t). */
extern const app_class_t puterm_app_class;

/* Queues a command to be typed into the *next* PUTerm window's shell the
 * instant it's created (as if the user had typed it themselves, followed
 * by Enter) -- e.g. PUFiles opening a text file spawns a PUTerm window
 * and calls this with "neatvi '/path/to/file'" first (see pude_spawn.h,
 * docs/pude.md's "Opening files" section). Consumed and cleared by the
 * very next puterm_create() call; leave unset (or pass NULL/"") for an
 * ordinary interactive shell. */
void puterm_set_startup_command(const char *command);

#endif
