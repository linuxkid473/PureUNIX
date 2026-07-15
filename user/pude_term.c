/* user/pude_term.c — PUTerm's terminal emulator core, plus (below) its
 * `app_class_t` wrapper (user/pude_app.h) turning it into a pluggable
 * `pude` app: a real pty (include/pureunix/pty.h), a forked+exec'd
 * BusyBox ash, and this file's own ANSI/VT100-subset interpreter and
 * SDL2 renderer. See pude_term.h and docs/pude.md. */
#include "pude_term.h"
#include "pude_gfx.h"
#include "pude_widgets.h"
#include "pureunix_pty.h"
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_FG 0xFF
#define DEFAULT_BG 0xFF

static void clear_cell(term_cell_t *c)
{
    c->ch = ' ';
    c->fg = DEFAULT_FG;
    c->bg = DEFAULT_BG;
    c->bold = 0;
    c->reverse = 0;
}

void term_init(term_t *t, int rows, int cols)
{
    memset(t, 0, sizeof(*t));
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    t->rows = rows;
    t->cols = cols;
    t->cursor_visible = 1;
    t->cur_fg = DEFAULT_FG;
    t->cur_bg = DEFAULT_BG;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            clear_cell(&t->cell[r][c]);
        }
    }
    t->dirty = 1;
}

void term_resize(term_t *t, int rows, int cols)
{
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == t->rows && cols == t->cols) {
        return;
    }

    /* Blank any newly-exposed cells -- both a wider grid (new columns on
     * existing rows) and a taller one (whole new rows) need it, since BSS/
     * static storage for cells beyond the old rows/cols may still hold
     * stale content from a previous, larger size. */
    for (int r = 0; r < rows; r++) {
        int start_c = (r < t->rows) ? t->cols : 0;
        for (int c = start_c; c < cols; c++) {
            clear_cell(&t->cell[r][c]);
        }
    }

    /* A shrink that pushes the cursor's row out of bounds clamps it onto
     * whatever row index it now maps to -- but that row's own content is
     * whatever was last written there (this grid is a fixed array of rows,
     * not a true scrolling FIFO), completely unrelated to "what comes
     * next". Left alone, the next character typed lands right after that
     * stale content instead of on a blank line -- confirmed as a real,
     * visible bug (docs/pude.md): shrinking the window mid-session made
     * new output visually glue onto an old, unrelated line still sitting
     * in the now-current row. Clearing that one row before clamping onto
     * it guarantees new output always starts clean, at the cost of
     * (already-documented, accepted) losing whatever was there — the same
     * "shrink simply clips" simplification real terminal emulators make,
     * just without the cosmetic glitch. */
    if (t->cur_row >= rows) {
        for (int c = 0; c < cols; c++) {
            clear_cell(&t->cell[rows - 1][c]);
        }
        t->cur_row = rows - 1;
        t->cur_col = 0;
    }

    t->rows = rows;
    t->cols = cols;
    /* Real terminal emulators reset the scroll region on resize rather
     * than trying to preserve a now-possibly-out-of-range DECSTBM region
     * — the redrawing program (ash, vi, ...) sets its own region again if
     * it needs one. */
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    if (t->cur_col >= cols) t->cur_col = cols - 1;
    t->sb_view = 0;
    t->dirty = 1;
}

static void push_scrollback(term_t *t, int row)
{
    for (int c = 0; c < t->cols; c++) {
        char ch = t->cell[row][c].ch;
        t->sb[t->sb_next][c] = ch ? ch : ' ';
    }
    for (int c = t->cols; c < TERM_MAX_COLS; c++) {
        t->sb[t->sb_next][c] = ' ';
    }
    t->sb_next = (t->sb_next + 1) % TERM_SCROLLBACK;
    if (t->sb_count < TERM_SCROLLBACK) {
        t->sb_count++;
    }
}

/* Any new output snaps the view back to live, exactly like a real terminal
 * (mirrors drivers/vga.c's vga_putc_vt()'s own sb_view-reset rule, see
 * docs/vt.md's Scrollback section). */
static void snap_to_live(term_t *t)
{
    t->sb_view = 0;
}

static void scroll_up_region(term_t *t)
{
    bool full_screen = (t->scroll_top == 0 && t->scroll_bottom == t->rows - 1);
    if (full_screen) {
        push_scrollback(t, t->scroll_top);
    }
    for (int r = t->scroll_top; r < t->scroll_bottom; r++) {
        for (int c = 0; c < t->cols; c++) {
            t->cell[r][c] = t->cell[r + 1][c];
        }
    }
    for (int c = 0; c < t->cols; c++) {
        clear_cell(&t->cell[t->scroll_bottom][c]);
    }
}

static void scroll_down_region(term_t *t)
{
    for (int r = t->scroll_bottom; r > t->scroll_top; r--) {
        for (int c = 0; c < t->cols; c++) {
            t->cell[r][c] = t->cell[r - 1][c];
        }
    }
    for (int c = 0; c < t->cols; c++) {
        clear_cell(&t->cell[t->scroll_top][c]);
    }
}

/* Real newline: scrolls the region if already at its bottom, then resets
 * the column -- matches drivers/vga.c's newline() (see docs/pude.md's
 * design note on why '\n' means full CRLF here, not bare LF). */
static void do_newline(term_t *t)
{
    if (t->cur_row >= t->scroll_bottom) {
        scroll_up_region(t);
    } else {
        t->cur_row++;
    }
    t->cur_col = 0;
}

/* ESC D (IND): scroll if needed, but column is untouched -- distinct from
 * '\n' (see pude_term.h). */
static void do_index(term_t *t)
{
    if (t->cur_row >= t->scroll_bottom) {
        scroll_up_region(t);
    } else {
        t->cur_row++;
    }
}

/* ESC M (RI): reverse index -- scroll the region down if at its top. */
static void do_reverse_index(term_t *t)
{
    if (t->cur_row <= t->scroll_top) {
        scroll_down_region(t);
    } else {
        t->cur_row--;
    }
}

static void put_char(term_t *t, char ch)
{
    if (t->cur_col >= t->cols) {
        do_newline(t);
    }
    term_cell_t *c = &t->cell[t->cur_row][t->cur_col];
    c->ch = ch;
    c->fg = t->cur_fg;
    c->bg = t->cur_bg;
    c->bold = t->cur_bold;
    c->reverse = t->cur_reverse;
    t->cur_col++;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Parses up to `max` semicolon-separated integer parameters from a CSI
 * sequence body (a leading '?' private marker, already noted via
 * t->csi_private, must be skipped by the caller before this is called).
 * A missing/empty parameter is reported as -1 so callers can tell "0" and
 * "omitted" apart (they default differently for some commands). Returns
 * the number of parameters found (0 if the body is empty). */
static int parse_params(const char *buf, int len, int *params, int max)
{
    int count = 0;
    int val = -1;
    bool have = false;
    for (int i = 0; i < len && count < max; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            if (!have) {
                val = 0;
                have = true;
            }
            val = val * 10 + (c - '0');
        } else if (c == ';') {
            params[count++] = have ? val : -1;
            val = -1;
            have = false;
        }
    }
    if (count < max) {
        params[count++] = have ? val : -1;
    }
    return count;
}

static void apply_sgr(term_t *t, const int *p, int n)
{
    if (n == 0) {
        /* Bare "\E[m" == "\E[0m" (reset). */
        t->cur_fg = DEFAULT_FG;
        t->cur_bg = DEFAULT_BG;
        t->cur_bold = 0;
        t->cur_reverse = 0;
        return;
    }
    for (int i = 0; i < n; i++) {
        int v = p[i] < 0 ? 0 : p[i];
        if (v == 0) {
            t->cur_fg = DEFAULT_FG;
            t->cur_bg = DEFAULT_BG;
            t->cur_bold = 0;
            t->cur_reverse = 0;
        } else if (v == 1) {
            t->cur_bold = 1;
        } else if (v == 22) {
            t->cur_bold = 0;
        } else if (v == 7) {
            t->cur_reverse = 1;
        } else if (v == 27) {
            t->cur_reverse = 0;
        } else if (v >= 30 && v <= 37) {
            t->cur_fg = (unsigned char)(v - 30);
        } else if (v == 39) {
            t->cur_fg = DEFAULT_FG;
        } else if (v >= 40 && v <= 47) {
            t->cur_bg = (unsigned char)(v - 40);
        } else if (v == 49) {
            t->cur_bg = DEFAULT_BG;
        } else if (v >= 90 && v <= 97) {
            t->cur_fg = (unsigned char)(8 + v - 90);
        } else if (v >= 100 && v <= 107) {
            t->cur_bg = (unsigned char)(8 + v - 100);
        }
        /* Any other SGR code (underline, blink, ...) is silently ignored —
         * drivers/vga.c's own ANSI parser doesn't implement them either. */
    }
}

static void erase_display(term_t *t, int mode)
{
    if (mode == 1) {
        for (int r = 0; r <= t->cur_row; r++) {
            int end = (r == t->cur_row) ? t->cur_col : t->cols - 1;
            for (int c = 0; c <= end && c < t->cols; c++) {
                clear_cell(&t->cell[r][c]);
            }
        }
    } else if (mode == 2) {
        for (int r = 0; r < t->rows; r++) {
            for (int c = 0; c < t->cols; c++) {
                clear_cell(&t->cell[r][c]);
            }
        }
    } else {
        /* mode 0 (default): cursor to end of screen. */
        for (int c = t->cur_col; c < t->cols; c++) {
            clear_cell(&t->cell[t->cur_row][c]);
        }
        for (int r = t->cur_row + 1; r < t->rows; r++) {
            for (int c = 0; c < t->cols; c++) {
                clear_cell(&t->cell[r][c]);
            }
        }
    }
}

static void erase_line(term_t *t, int mode)
{
    int r = t->cur_row;
    if (mode == 1) {
        for (int c = 0; c <= t->cur_col && c < t->cols; c++) {
            clear_cell(&t->cell[r][c]);
        }
    } else if (mode == 2) {
        for (int c = 0; c < t->cols; c++) {
            clear_cell(&t->cell[r][c]);
        }
    } else {
        for (int c = t->cur_col; c < t->cols; c++) {
            clear_cell(&t->cell[r][c]);
        }
    }
}

static void dispatch_csi(term_t *t, char final)
{
    int len = t->csi_len;
    const char *body = t->csi_buf;
    if (len > 0 && body[0] == '?') {
        t->csi_private = 1;
        body++;
        len--;
    }
    int p[8];
    int n = parse_params(body, len, p, 8);
    int p0 = (n > 0 && p[0] >= 0) ? p[0] : -1;

    switch (final) {
    case 'A': t->cur_row = clampi(t->cur_row - (p0 < 0 ? 1 : p0), 0, t->rows - 1); break;
    case 'B': t->cur_row = clampi(t->cur_row + (p0 < 0 ? 1 : p0), 0, t->rows - 1); break;
    case 'C': t->cur_col = clampi(t->cur_col + (p0 < 0 ? 1 : p0), 0, t->cols - 1); break;
    case 'D': t->cur_col = clampi(t->cur_col - (p0 < 0 ? 1 : p0), 0, t->cols - 1); break;
    case 'H':
    case 'f': {
        int row = (n > 0 && p[0] > 0) ? p[0] - 1 : 0;
        int col = (n > 1 && p[1] > 0) ? p[1] - 1 : 0;
        t->cur_row = clampi(row, 0, t->rows - 1);
        t->cur_col = clampi(col, 0, t->cols - 1);
        break;
    }
    case 'J': erase_display(t, p0 < 0 ? 0 : p0); break;
    case 'K': erase_line(t, p0 < 0 ? 0 : p0); break;
    case 'L': {
        int cnt = p0 < 0 ? 1 : p0;
        for (int i = 0; i < cnt; i++) scroll_down_region(t);
        break;
    }
    case 'M': {
        int cnt = p0 < 0 ? 1 : p0;
        for (int i = 0; i < cnt; i++) scroll_up_region(t);
        break;
    }
    case 'm': apply_sgr(t, p, n); break;
    case 'r': {
        int top = (n > 0 && p[0] > 0) ? p[0] - 1 : 0;
        int bot = (n > 1 && p[1] > 0) ? p[1] - 1 : t->rows - 1;
        if (top < bot && bot < t->rows) {
            t->scroll_top = top;
            t->scroll_bottom = bot;
        }
        break;
    }
    case 'h':
        if (t->csi_private && p0 == 25) {
            t->cursor_visible = 1;
        }
        break;
    case 'l':
        if (t->csi_private && p0 == 25) {
            t->cursor_visible = 0;
        }
        break;
    default:
        /* Unimplemented CSI final byte -- silently ignored, matching
         * drivers/vga.c's own "unknown escape sequence" handling. */
        break;
    }
}

void term_feed(term_t *t, const char *data, size_t len)
{
    if (len > 0) {
        t->dirty = 1;
        snap_to_live(t);
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)data[i];

        if (t->state == TS_NORMAL) {
            if (b == 0x1b) {
                t->state = TS_ESC;
            } else if (b == '\n') {
                do_newline(t);
            } else if (b == '\r') {
                t->cur_col = 0;
            } else if (b == '\b') {
                if (t->cur_col > 0) t->cur_col--;
            } else if (b == '\t') {
                int next = (t->cur_col / 8 + 1) * 8;
                t->cur_col = clampi(next, 0, t->cols - 1);
            } else if (b == '\a') {
                /* bell -- no-op, same as drivers/vga.c */
            } else if (b >= 0x20 && b < 0x7f) {
                put_char(t, (char)b);
            }
            /* Other control bytes are silently dropped. */
        } else if (t->state == TS_ESC) {
            if (b == '[') {
                t->state = TS_CSI;
                t->csi_len = 0;
                t->csi_private = 0;
            } else if (b == 'D') {
                do_index(t);
                t->state = TS_NORMAL;
            } else if (b == 'M') {
                do_reverse_index(t);
                t->state = TS_NORMAL;
            } else if (b == '7') {
                t->saved_row = t->cur_row;
                t->saved_col = t->cur_col;
                t->state = TS_NORMAL;
            } else if (b == '8') {
                t->cur_row = clampi(t->saved_row, 0, t->rows - 1);
                t->cur_col = clampi(t->saved_col, 0, t->cols - 1);
                t->state = TS_NORMAL;
            } else {
                /* Unrecognized escape (e.g. a lone ESC, or one this subset
                 * doesn't implement) -- drop it and resync to normal. */
                t->state = TS_NORMAL;
            }
        } else { /* TS_CSI */
            if (b >= 0x40 && b <= 0x7e) {
                dispatch_csi(t, (char)b);
                t->state = TS_NORMAL;
            } else if (t->csi_len < (int)sizeof(t->csi_buf) - 1) {
                t->csi_buf[t->csi_len++] = (char)b;
            }
            /* An over-long CSI body just stops accumulating and waits for
             * its final byte -- never corrupts adjacent state. */
        }
    }
}

void term_scroll_view(term_t *t, int delta)
{
    t->sb_view += delta;
    if (t->sb_view < 0) t->sb_view = 0;
    if (t->sb_view > t->sb_count) t->sb_view = t->sb_count;
    t->dirty = 1;
}

/* ==========================================================================
 * PUTerm as a `pude` app (user/pude_app.h). Everything below used to live
 * directly in user/pude.c's single-window main() -- moved here unchanged
 * in behavior so PUTerm becomes an ordinary pluggable app, spawnable more
 * than once (each instance owns an independent pty + forked shell +
 * term_t), while user/pude.c itself no longer knows anything
 * PUTerm-specific.
 * ========================================================================== */

/* ---- 16-color ANSI palette (matches drivers/vga.c's own SGR handling) --- */
static const uint32_t ansi_palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};
#define DEFAULT_FG_RGB 0xC0C0C0
#define DEFAULT_BG_RGB 0x000000

static uint32_t color_for(unsigned char idx, uint32_t def)
{
    if (idx == 0xFF) {
        return def;
    }
    return ansi_palette[idx & 0x0F];
}

/* Smallest a drag-resize can shrink a PUTerm window to, in cells --
 * derived into pixels via puterm_app_class.min_client_w/h below. */
#define MIN_COLS 20
#define MIN_ROWS 4

/* ---- Keyboard encoding ---------------------------------------------------
 * PUTerm's own key->byte(s) translation, the input half of a real terminal
 * emulator (mirrors real xterm's job of turning X11 keysyms into pty
 * writes). Escape sequences sent for special keys match
 * third_party/ncurses/pureunix.terminfo's kcuu1/kcud1/.../kf1..kf12 exactly
 * -- the same bytes drivers/tty.c's key_to_escape_seq() produces for the
 * physical console, so ncurses/vi/htop running under PUTerm see identical
 * input to what they'd see on a real VT. */

/* Returns the number of bytes written into `out` (0 if this scancode
 * produces nothing PUTerm forwards -- e.g. a bare modifier key). */
static int encode_key(SDL_Scancode sc, key_mods_t mods, char *out)
{
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
        out[0] = '\r';
        return 1;
    }
    if (sc == SDL_SCANCODE_BACKSPACE) {
        out[0] = 0x7F;
        return 1;
    }
    if (sc == SDL_SCANCODE_TAB) {
        out[0] = '\t';
        return 1;
    }
    if (sc == SDL_SCANCODE_ESCAPE) {
        out[0] = 0x1B;
        return 1;
    }
    if (sc == SDL_SCANCODE_UP)    { memcpy(out, "\033[A", 3); return 3; }
    if (sc == SDL_SCANCODE_DOWN)  { memcpy(out, "\033[B", 3); return 3; }
    if (sc == SDL_SCANCODE_RIGHT) { memcpy(out, "\033[C", 3); return 3; }
    if (sc == SDL_SCANCODE_LEFT)  { memcpy(out, "\033[D", 3); return 3; }
    if (sc == SDL_SCANCODE_HOME)  { memcpy(out, "\033[H", 3); return 3; }
    if (sc == SDL_SCANCODE_END)   { memcpy(out, "\033[F", 3); return 3; }
    if (sc == SDL_SCANCODE_PAGEUP)   { memcpy(out, "\033[5~", 4); return 4; }
    if (sc == SDL_SCANCODE_PAGEDOWN) { memcpy(out, "\033[6~", 4); return 4; }
    if (sc == SDL_SCANCODE_DELETE)   { memcpy(out, "\033[3~", 4); return 4; }
    if (sc == SDL_SCANCODE_F1) { memcpy(out, "\033OP", 3); return 3; }
    if (sc == SDL_SCANCODE_F2) { memcpy(out, "\033OQ", 3); return 3; }
    if (sc == SDL_SCANCODE_F3) { memcpy(out, "\033OR", 3); return 3; }
    if (sc == SDL_SCANCODE_F4) { memcpy(out, "\033OS", 3); return 3; }
    if (sc == SDL_SCANCODE_F5)  { memcpy(out, "\033[15~", 5); return 5; }
    if (sc == SDL_SCANCODE_F6)  { memcpy(out, "\033[17~", 5); return 5; }
    if (sc == SDL_SCANCODE_F7)  { memcpy(out, "\033[18~", 5); return 5; }
    if (sc == SDL_SCANCODE_F8)  { memcpy(out, "\033[19~", 5); return 5; }
    if (sc == SDL_SCANCODE_F9)  { memcpy(out, "\033[20~", 5); return 5; }
    if (sc == SDL_SCANCODE_F10) { memcpy(out, "\033[21~", 5); return 5; }
    if (sc == SDL_SCANCODE_F11) { memcpy(out, "\033[23~", 5); return 5; }
    if (sc == SDL_SCANCODE_F12) { memcpy(out, "\033[24~", 5); return 5; }

    char base = pu_scancode_to_ascii(sc, mods);
    if (base != 0) {
        if (mods.ctrl && base >= 'a' && base <= 'z') {
            out[0] = (char)(base - 'a' + 1); /* Ctrl+letter -> control byte */
        } else if (mods.ctrl && base >= 'A' && base <= 'Z') {
            out[0] = (char)(base - 'A' + 1);
        } else {
            out[0] = base;
        }
        return 1;
    }
    return 0;
}

/* ---- Rendering ------------------------------------------------------------
 * Renders the terminal's current view (live grid, or scrolled-back history
 * if term->sb_view > 0 -- Shift+PageUp/PageDown, mirroring kernel/vt.c's
 * own scrollback convention, see docs/vt.md) into the window surface at
 * (ox,oy). */
static void render_term(SDL_Surface *s, term_t *t, int ox, int oy)
{
    int cw = FONT_CELL_W, ch = FONT_CELL_H;

    for (int vr = 0; vr < t->rows; vr++) {
        int logical = (t->sb_count - t->sb_view) + vr;
        int y = oy + vr * ch;

        if (t->sb_view > 0 && logical < t->sb_count) {
            /* Plain-text history line -- no per-cell attributes, same
             * simplification kernel/vt.c's own scrollback makes. */
            int phys = (t->sb_next - t->sb_count + logical + TERM_SCROLLBACK) % TERM_SCROLLBACK;
            for (int c = 0; c < t->cols; c++) {
                pu_draw_glyph(s, ox + c * cw, y, t->sb[phys][c], DEFAULT_FG_RGB, DEFAULT_BG_RGB);
            }
            continue;
        }

        int live_row = logical - t->sb_count;
        if (live_row < 0 || live_row >= t->rows) {
            for (int c = 0; c < t->cols; c++) {
                pu_draw_glyph(s, ox + c * cw, y, ' ', DEFAULT_FG_RGB, DEFAULT_BG_RGB);
            }
            continue;
        }

        for (int c = 0; c < t->cols; c++) {
            term_cell_t *cell = &t->cell[live_row][c];
            uint32_t fg = color_for(cell->fg, DEFAULT_FG_RGB);
            uint32_t bg = color_for(cell->bg, DEFAULT_BG_RGB);
            if (cell->reverse) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            bool is_cursor = (t->sb_view == 0 && t->cursor_visible &&
                              live_row == t->cur_row && c == t->cur_col);
            if (is_cursor) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            pu_draw_glyph(s, ox + c * cw, y, cell->ch ? cell->ch : ' ', fg, bg);
        }
    }
}

/* ---- Child shell lifecycle ------------------------------------------------ */

typedef struct {
    term_t *term;
    int master_fd;
    pid_t child;
    bool child_alive;
} puterm_state_t;

/* See puterm_set_startup_command()'s doc comment in pude_term.h. A plain
 * static, not a mailbox like pude_spawn.h's -- there is exactly one
 * producer (whichever app called this right before requesting the
 * spawn) and exactly one consumer (the very next puterm_create()), so a
 * one-shot buffer is all that's needed. */
static char g_startup_command[256];
static bool g_has_startup_command;

void puterm_set_startup_command(const char *command)
{
    if (command && command[0]) {
        strncpy(g_startup_command, command, sizeof(g_startup_command) - 1);
        g_startup_command[sizeof(g_startup_command) - 1] = '\0';
        g_has_startup_command = true;
    } else {
        g_has_startup_command = false;
    }
}

static pid_t spawn_shell(int slave_fd)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setsid();
        if (ioctl(slave_fd, TIOCSCTTY, NULL) != 0) {
            _exit(126);
        }
        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        if (slave_fd > 2) {
            close(slave_fd);
        }

        extern char **environ;
        char *envp[32];
        int n = 0;
        for (char **e = environ; e && *e && n < 30; e++, n++) {
            envp[n] = *e;
        }
        envp[n++] = (char *)"TERM=pureunix";
        envp[n] = NULL;

        char *argv[] = { (char *)"sh", NULL };
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    return pid;
}

static void *puterm_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    int cols = client_w / FONT_CELL_W;
    int rows = client_h / FONT_CELL_H;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    puterm_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    /* Not a local: term_t (cell grid + 500-line scrollback) is ~190 KiB --
     * see pude_term.h/docs/pude.md on why this must never be a stack
     * local in a kernel with no demand-paged/guard-page stack growth. A
     * heap allocation (this kernel's real incremental sbrk(), added
     * during the Chocolate Doom port) is what makes more than one
     * concurrent instance possible at all -- the original single-window
     * pude used a single `static term_t` precisely because there was only
     * ever one. */
    st->term = malloc(sizeof(term_t));
    if (!st->term) {
        free(st);
        return NULL;
    }
    term_init(st->term, rows, cols);

    int pty_fds[2];
    if (pu_pty_create(pty_fds) != 0) {
        free(st->term);
        free(st);
        return NULL;
    }
    st->master_fd = pty_fds[0];
    int slave_fd = pty_fds[1];

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(st->master_fd, TIOCSWINSZ, &ws);

    st->child = spawn_shell(slave_fd);
    if (st->child < 0) {
        close(st->master_fd);
        close(slave_fd);
        free(st->term);
        free(st);
        return NULL;
    }
    close(slave_fd); /* PUTerm only needs the master end */
    st->child_alive = true;

    if (g_has_startup_command) {
        /* Typed into the pty exactly as if the user had done it themselves
         * -- the freshly forked shell just hasn't printed its prompt yet
         * when this write happens, but the pty's own buffering means the
         * bytes sit there until it's ready to read them, same as a human
         * typing ahead of a slow prompt. */
        write(st->master_fd, g_startup_command, strlen(g_startup_command));
        write(st->master_fd, "\n", 1);
        g_has_startup_command = false;
    }
    return st;
}

static void puterm_destroy(pude_window_t *win, void *state)
{
    (void)win;
    puterm_state_t *st = state;
    if (st->child_alive) {
        /* A clean hangup, mirroring what closing a real terminal
         * emulator's window does to the process group it owns. */
        kill(-st->child, SIGHUP);
        int status = 0;
        waitpid(st->child, &status, 0);
    }
    close(st->master_fd);
    free(st->term);
    free(st);
}

static void puterm_render(pude_window_t *win, void *state, SDL_Surface *s,
                           int cx, int cy, int cw, int ch)
{
    (void)win;
    (void)cw;
    (void)ch;
    puterm_state_t *st = state;
    pu_fill_rect(s, cx, cy, st->term->cols * FONT_CELL_W, st->term->rows * FONT_CELL_H,
                 SDL_MapRGB(s->format, 0, 0, 0));
    render_term(s, st->term, cx, cy);
}

static void puterm_on_key(pude_window_t *win, void *state, SDL_Scancode sc,
                          key_mods_t mods, bool down)
{
    (void)win;
    if (!down) {
        return;
    }
    puterm_state_t *st = state;
    if (mods.shift && sc == SDL_SCANCODE_PAGEUP) {
        term_scroll_view(st->term, 1);
        return;
    }
    if (mods.shift && sc == SDL_SCANCODE_PAGEDOWN) {
        term_scroll_view(st->term, -1);
        return;
    }
    char bytes[8];
    int n = encode_key(sc, mods, bytes);
    if (n > 0 && st->child_alive) {
        write(st->master_fd, bytes, (size_t)n);
    }
}

static void puterm_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    puterm_state_t *st = state;
    int new_cols = new_client_w / FONT_CELL_W;
    int new_rows = new_client_h / FONT_CELL_H;
    term_resize(st->term, new_rows, new_cols);
    struct winsize ws = { (unsigned short)st->term->rows, (unsigned short)st->term->cols, 0, 0 };
    ioctl(st->master_fd, TIOCSWINSZ, &ws);
}

static bool puterm_poll(pude_window_t *win, void *state)
{
    (void)win;
    puterm_state_t *st = state;
    bool got_output = false;

    if (st->child_alive) {
        int status = 0;
        pid_t r = waitpid(st->child, &status, WNOHANG);
        if (r == st->child) {
            st->child_alive = false;
        }
    }

    char io_buf[4096];
    for (;;) {
        int n = (int)read(st->master_fd, io_buf, sizeof(io_buf));
        if (n <= 0) {
            break;
        }
        term_feed(st->term, io_buf, (size_t)n);
        got_output = true;
    }

    if (got_output || st->term->dirty) {
        st->term->dirty = 0;
        return true;
    }
    return false;
}

static bool puterm_is_alive(pude_window_t *win, void *state)
{
    (void)win;
    puterm_state_t *st = state;
    return st->child_alive;
}

const app_class_t puterm_app_class = {
    .name = "PUTerm",
    .default_client_w = 80 * FONT_CELL_W,
    .default_client_h = 24 * FONT_CELL_H,
    .min_client_w = MIN_COLS * FONT_CELL_W,
    .min_client_h = MIN_ROWS * FONT_CELL_H,
    .create = puterm_create,
    .destroy = puterm_destroy,
    .render = puterm_render,
    .on_key = puterm_on_key,
    .on_mouse_down = NULL,
    .on_mouse_up = NULL,
    .on_resize = puterm_on_resize,
    .poll = puterm_poll,
    .is_alive = puterm_is_alive,
    .icon_draw = pu_icon_puterm,
    .graphical = true,
    .pinned_default = true,
};
