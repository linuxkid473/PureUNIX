#include <pureunix/framebuffer.h>
#include <pureunix/font.h>
#include <pureunix/io.h>
#include <pureunix/serial.h>
#include <pureunix/string.h>
#include <pureunix/vga.h>

/* Legacy hardware VGA text mode (no framebuffer available) is always a real
 * 80x25 character device — that's fixed by the hardware, not a choice this
 * driver makes, so it keeps its own constants distinct from the
 * framebuffer-backed grid's runtime vga_cols/vga_rows below. */
#define TEXT_MODE_COLS 80
#define TEXT_MODE_ROWS 25

/* Upper bound on the framebuffer-backed grid, sized generously for the
 * largest cols x rows combination realistic hardware can hit at the
 * smallest supported font scale (1x, FONT_CELL_W x FONT_CELL_H): a 4K-wide
 * display (3840px / 8px) needs ~480 columns, a tall display (2160px / 17px
 * plus margin) needs well under 128 rows. Static storage (not kmalloc'd)
 * because vga_init() runs before heap_init() (see kernel/main.c) so early
 * boot output has a console before the heap exists.
 */
#define MAX_COLS 480
#define MAX_ROWS 128

/* Scrollback: text-only (no attributes — a modest fidelity cut to keep the
 * per-console footprint down at VT_COUNT consoles each carrying their own
 * copy) ring of lines evicted from the top of the visible grid by scroll()
 * below. See vga_console_scrollback_view() for how it's rendered. */
#define SCROLLBACK_LINES 150

/* One independent console: everything scroll()/set_cell()/the ANSI parser
 * touch. A physical display only ever has one of these actually driving
 * pixels/hardware at a time (see g_active below) — every other console
 * silently keeps its own cell_char/cell_attr/cursor/parser state up to date
 * from whatever's being written to it (drivers/tty.c's echo, a background
 * task's SYS_WRITE, ...) so switching to it later is a pure repaint from
 * already-correct state, never a re-render of history. This is what makes
 * kernel/vt.c's "one struct per VT, redraw on switch, no N framebuffer
 * copies" design possible. */
typedef struct console {
    char cell_char[MAX_ROWS][MAX_COLS];
    uint8_t cell_attr[MAX_ROWS][MAX_COLS];
    size_t row;
    size_t col;
    uint8_t color;
    int esc_state;
    char esc_buf[16];
    size_t esc_len;
    /* DECSTBM scroll region, 0-indexed and inclusive on both ends. */
    int scroll_top;
    int scroll_bottom;
    /* Tracks where the software cursor overlay is currently drawn on the
     * framebuffer for *this* console the last time it was active, so it can
     * be erased before being redrawn at its new position. Meaningless (and
     * never consulted) while this console isn't g_active. */
    size_t cur_row;
    size_t cur_col;
    bool cursor_valid;
    /* Scrollback ring: sb_char[sb_next] is the next slot to overwrite;
     * sb_count is how many of the SCROLLBACK_LINES slots hold real history
     * (caps at SCROLLBACK_LINES, then every push overwrites the oldest).
     * sb_view is how many lines the viewport is currently scrolled back by
     * (0 = live, showing cell_char as normal) — see
     * vga_console_scroll_view(). */
    char sb_char[SCROLLBACK_LINES][MAX_COLS];
    size_t sb_count;
    size_t sb_next;
    size_t sb_view;
} console_t;

#define VGA_MAX_CONSOLES 8

static const uint32_t vga_rgb_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static volatile uint16_t *const vga_memory = (uint16_t *)0xB8000;

/* Console storage: a small fixed pool (VGA_MAX_CONSOLES), not per-console
 * kmalloc — same fixed-resource style as everything else in this kernel,
 * and vga_init() (console 0, the "boot console") runs before heap_init()
 * anyway. kernel/vt.c claims console 0 as VT1 and consoles 1..VT_COUNT-1 as
 * VT2..VT_COUNT via vga_console(); nothing else should reach past
 * VT_COUNT-1's index into this pool. */
static console_t g_consoles[VGA_MAX_CONSOLES];
/* The console currently driving real hardware (framebuffer pixels / VGA
 * text-mode memory / the hardware cursor / serial mirroring). Every other
 * console in g_consoles[] just keeps its own cell_char/cell_attr/cursor
 * state current without touching any of that — see force_draw() below. */
static console_t *g_active = &g_consoles[0];

/* Hardware-wide properties: the same physical display, grid size, and font
 * geometry are shared by every console (only one is ever actually on
 * screen), so these stay global rather than per-console. */
static bool use_fb;
/* Pixel origin of the text grid within the framebuffer. Bootloaders grant a
 * framebuffer larger than requested; centered, see vga_compute_geometry(). */
static uint32_t origin_x;
static uint32_t origin_y;
static size_t vga_cols = TEXT_MODE_COLS;
static size_t vga_rows = TEXT_MODE_ROWS;

/* ---- Perf instrumentation -------------------------------------------------
 * Cheap always-on counters for the metrics this console's redraw path was
 * suspected of getting wrong on real hardware: glyphs actually rendered,
 * bytes actually moved, and cycles spent in each. Only ever incremented for
 * g_active — a background console's logical-only updates do no rendering
 * work to measure. */
static uint32_t perf_glyphs;
static uint32_t perf_glyph_cycles;
static uint32_t perf_scroll_calls;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void perf_report(const char *label, uint32_t rows, uint32_t bytes, uint32_t cycles)
{
    serial_write("\r\nPERF ");
    serial_write(label);
    serial_write(" rows=");
    serial_write_uint(rows);
    serial_write(" bytes=");
    serial_write_uint(bytes);
    serial_write(" cyc=");
    serial_write_uint(cycles);
    serial_write(" glyphs=");
    serial_write_uint(perf_glyphs);
    serial_write(" glyph_cyc=");
    serial_write_uint(perf_glyph_cycles);
    serial_write("\r\n");
    perf_glyphs = 0;
    perf_glyph_cycles = 0;
}

static uint16_t vga_entry(char c, uint8_t entry_color)
{
    return (uint16_t)c | ((uint16_t)entry_color << 8);
}

static uint8_t make_color(uint8_t fg, uint8_t bg)
{
    return fg | (bg << 4);
}

uint8_t vga_default_color(void)
{
    return make_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Sets a console's current SGR color outright, bypassing the ANSI parser --
 * used only to seed a brand-new console with a sane starting color (see
 * vga_console(): the g_consoles[] pool is BSS, zero-initialized, which
 * happens to be make_color(VGA_BLACK, VGA_BLACK) -- invisible black-on-black
 * text, not "blank", if nothing ever sets a real color first). Runtime
 * color changes always go through ansi_sgr() instead; this deliberately
 * doesn't touch anything else (row/col/cursor/buffer), unlike
 * vga_console_reset(), since it's meant to run once, before that. */
void vga_console_set_color(console_t *cs, uint8_t color)
{
    cs->color = color;
}

console_t *vga_console(int n)
{
    if (n < 0 || n >= VGA_MAX_CONSOLES) {
        return NULL;
    }
    return &g_consoles[n];
}

console_t *vga_active_console(void)
{
    return g_active;
}

bool vga_console_is_active(const console_t *cs)
{
    return cs == g_active;
}

/* Renders one glyph at scale font_get_scale(): each base glyph pixel becomes
 * an SxS block of framebuffer pixels (nearest-neighbor upscale). Only ever
 * called for cs == g_active (see force_draw()) — never wastes real pixel
 * work on a console that isn't on screen. */
static void draw_cell_ex(console_t *cs, size_t r, size_t c, char ch, uint8_t attr, bool invert)
{
    (void)cs;
    uint64_t t0 = rdtsc();
    uint32_t fg = vga_rgb_palette[invert ? (attr >> 4) & 0xF : attr & 0xF];
    uint32_t bg = vga_rgb_palette[invert ? attr & 0xF : (attr >> 4) & 0xF];
    const uint8_t *glyph = font_glyph(ch);
    int scale = font_get_scale();
    uint32_t px0 = origin_x + (uint32_t)c * (uint32_t)font_cell_w();
    uint32_t py0 = origin_y + (uint32_t)r * (uint32_t)font_cell_h();
    for (uint32_t y = 0; y < FONT_CELL_H; ++y) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_CELL_W; ++x) {
            uint32_t px = (bits & (0x80 >> x)) ? fg : bg;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    fb_put_pixel(px0 + x * (uint32_t)scale + (uint32_t)sx,
                                 py0 + y * (uint32_t)scale + (uint32_t)sy, px);
                }
            }
        }
    }
    perf_glyphs++;
    perf_glyph_cycles += (uint32_t)(rdtsc() - t0);
}

static void draw_cell(console_t *cs, size_t r, size_t c)
{
    draw_cell_ex(cs, r, c, cs->cell_char[r][c], cs->cell_attr[r][c], false);
}

/* Unconditionally repaints (r, c) from cs's current cell_char/cell_attr,
 * bypassing set_cell()'s change check. No-op for any console that isn't
 * g_active: its backing array is still updated by the caller (that's the
 * whole point of a background console), there's just no hardware to paint
 * it onto right now — kernel/vt.c's vt_switch() does one full repaint pass
 * over the newly active console instead when it takes over. */
static void force_draw(console_t *cs, size_t r, size_t c)
{
    if (cs != g_active) {
        return;
    }
    if (use_fb) {
        draw_cell(cs, r, c);
    } else {
        vga_memory[r * vga_cols + c] = vga_entry(cs->cell_char[r][c], cs->cell_attr[r][c]);
    }
}

/* Skips the redraw entirely when neither the character nor its attribute
 * actually changed for this console — the single highest-leverage fix for
 * avoiding unnecessary redraw work (see force_draw() for the g_active gate
 * that separately skips hardware work for a backgrounded console). */
static void set_cell(console_t *cs, size_t r, size_t c, char ch, uint8_t attr)
{
    if (cs->cell_char[r][c] == ch && cs->cell_attr[r][c] == attr) {
        return;
    }
    cs->cell_char[r][c] = ch;
    cs->cell_attr[r][c] = attr;
    force_draw(cs, r, c);
}

static void cursor_restore(console_t *cs)
{
    if (cs != g_active) {
        return;
    }
    if (use_fb && cs->cursor_valid) {
        draw_cell(cs, cs->cur_row, cs->cur_col);
    }
}

static void cursor_draw(console_t *cs)
{
    if (cs != g_active) {
        return;
    }
    if (use_fb) {
        draw_cell_ex(cs, cs->row, cs->col, cs->cell_char[cs->row][cs->col],
                     cs->cell_attr[cs->row][cs->col], true);
        cs->cur_row = cs->row;
        cs->cur_col = cs->col;
        cs->cursor_valid = true;
    }
}

static void update_cursor(console_t *cs)
{
    if (cs != g_active) {
        return;
    }
    if (use_fb) {
        cursor_restore(cs);
        cursor_draw(cs);
        return;
    }
    uint16_t pos = (uint16_t)(cs->row * vga_cols + cs->col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

/* Pushes the line about to scroll off the top of the *whole screen* into
 * cs's scrollback ring. Only called for the default full-screen scroll
 * region (scroll_top == 0 && scroll_bottom == vga_rows-1) — a custom
 * DECSTBM region (neatvi confining its own scroll to everything above a
 * fixed status line, see user/vi/term.c) is editor-internal redraw noise,
 * not real scrollback-worthy output. */
static void scrollback_push(console_t *cs, const char *line)
{
    memcpy(cs->sb_char[cs->sb_next], line, vga_cols < MAX_COLS ? vga_cols : MAX_COLS);
    if (vga_cols < MAX_COLS) {
        memset(cs->sb_char[cs->sb_next] + vga_cols, 0, MAX_COLS - vga_cols);
    }
    cs->sb_next = (cs->sb_next + 1) % SCROLLBACK_LINES;
    if (cs->sb_count < SCROLLBACK_LINES) {
        cs->sb_count++;
    }
}

/* Shifts [scroll_top, scroll_bottom] up by one line, blanking the newly
 * exposed bottom line. Rows outside the region are left untouched. Runs the
 * logical (cell_char/cell_attr) update unconditionally — every console
 * keeps its own buffer correct regardless of whether it's on screen — and
 * gates the pixel-level work (fb_scroll_up()/vga_memory shift/perf
 * reporting) on cs == g_active via the same reasoning as force_draw(). */
static void scroll(console_t *cs)
{
    int top = cs->scroll_top;
    int bot = cs->scroll_bottom;
    if (bot <= top) {
        return;
    }
    if (top == 0 && bot == (int)vga_rows - 1) {
        scrollback_push(cs, cs->cell_char[top]);
    }
    uint64_t t0 = rdtsc();
    uint32_t bytes = 0;
    for (int y = top; y < bot; ++y) {
        memmove(cs->cell_char[y], cs->cell_char[y + 1], vga_cols);
        memmove(cs->cell_attr[y], cs->cell_attr[y + 1], vga_cols);
    }
    bytes += (uint32_t)((bot - top) * (long)vga_cols * 2);
    for (size_t x = 0; x < vga_cols; ++x) {
        cs->cell_char[bot][x] = ' ';
        cs->cell_attr[bot][x] = cs->color;
    }
    if (cs != g_active) {
        return;
    }
    if (use_fb) {
        uint32_t cw = (uint32_t)font_cell_w();
        uint32_t ch = (uint32_t)font_cell_h();
        fb_scroll_up(origin_x, origin_y + (uint32_t)top * ch,
                     (uint32_t)vga_cols * cw, (uint32_t)(bot - top + 1) * ch,
                     ch, vga_rgb_palette[(cs->color >> 4) & 0xF]);
        bytes += (uint32_t)(bot - top) * ch * (uint32_t)vga_cols * cw * (fb_get_info()->bpp / 8);
        if (cs->cursor_valid && (int)cs->cur_row >= top && (int)cs->cur_row <= bot) {
            if ((int)cs->cur_row == top) {
                cs->cursor_valid = false;
            } else {
                cs->cur_row--;
            }
        }
    } else {
        for (int y = top + 1; y <= bot; ++y) {
            for (size_t x = 0; x < vga_cols; ++x) {
                vga_memory[(size_t)(y - 1) * vga_cols + x] = vga_memory[(size_t)y * vga_cols + x];
            }
        }
        for (size_t x = 0; x < vga_cols; ++x) {
            vga_memory[(size_t)bot * vga_cols + x] = vga_entry(' ', cs->color);
        }
        bytes += (uint32_t)((bot - top) * (long)vga_cols * 2);
    }
    uint32_t cycles = (uint32_t)(rdtsc() - t0);
    perf_scroll_calls++;
    perf_report("scroll", (uint32_t)(bot - top + 1), bytes, cycles);
}

/* Inserts/deletes |n| blank lines at the cursor row, shifting the rest of
 * the scroll region down/up (VT100 IL/DL). Always a full redraw of the
 * affected rows via force_draw() (which self-gates on g_active), not
 * scroll()'s memmove+fb_scroll_up fast path. */
static void insert_lines(console_t *cs, int n)
{
    int top = (int)cs->row;
    int bot = cs->scroll_bottom;
    if (top > bot || n <= 0) {
        return;
    }
    if (n > bot - top + 1) {
        n = bot - top + 1;
    }
    for (int y = bot; y >= top + n; --y) {
        memcpy(cs->cell_char[y], cs->cell_char[y - n], vga_cols);
        memcpy(cs->cell_attr[y], cs->cell_attr[y - n], vga_cols);
    }
    for (int y = top; y < top + n; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            cs->cell_char[y][x] = ' ';
            cs->cell_attr[y][x] = cs->color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            force_draw(cs, (size_t)y, x);
        }
    }
}

static void delete_lines(console_t *cs, int n)
{
    int top = (int)cs->row;
    int bot = cs->scroll_bottom;
    if (top > bot || n <= 0) {
        return;
    }
    if (n > bot - top + 1) {
        n = bot - top + 1;
    }
    for (int y = top; y <= bot - n; ++y) {
        memcpy(cs->cell_char[y], cs->cell_char[y + n], vga_cols);
        memcpy(cs->cell_attr[y], cs->cell_attr[y + n], vga_cols);
    }
    for (int y = bot - n + 1; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            cs->cell_char[y][x] = ' ';
            cs->cell_attr[y][x] = cs->color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            force_draw(cs, (size_t)y, x);
        }
    }
}

static void newline(console_t *cs)
{
    cs->col = 0;
    if ((int)cs->row == cs->scroll_bottom) {
        scroll(cs);
    } else if (cs->row + 1 < vga_rows) {
        cs->row++;
    }
}

static void erase_current_line(console_t *cs)
{
    if (cs == g_active) {
        serial_erase_line();
    }
    for (size_t x = cs->col; x < vga_cols; ++x) {
        set_cell(cs, cs->row, x, ' ', cs->color);
    }
}

/* ANSI SGR color numbers (black,red,green,yellow,blue,magenta,cyan,white)
 * count in a different order than the VGA/CGA palette this driver indexes
 * directly — red/blue and yellow/cyan are swapped. */
static const uint8_t ansi_to_vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

static void ansi_sgr(console_t *cs, const char *seq)
{
    int value = 0;
    bool have_value = false;
    for (size_t i = 0;; ++i) {
        char c = seq[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            have_value = true;
            continue;
        }
        if (c == ';' || c == 'm' || c == '\0') {
            if (!have_value || value == 0) {
                cs->color = vga_default_color();
            } else if (value >= 30 && value <= 37) {
                cs->color = make_color(ansi_to_vga[value - 30], cs->color >> 4);
            } else if (value >= 90 && value <= 97) {
                cs->color = make_color((uint8_t)(ansi_to_vga[value - 90] + 8), cs->color >> 4);
            } else if (value >= 40 && value <= 47) {
                cs->color = make_color(cs->color & 0x0F, ansi_to_vga[value - 40]);
            }
            value = 0;
            have_value = false;
        }
        if (c == 'm' || c == '\0') {
            break;
        }
    }
}

/* Parses up to `max` semicolon-separated decimal parameters from seq[0..n) —
 * pure parsing, touches no console state. */
static int ansi_params(const char *seq, size_t n, int *out, int max)
{
    int count = 0;
    int val = 0;
    bool have = false;
    for (size_t i = 0; i <= n; ++i) {
        char c = i < n ? seq[i] : ';';
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            have = true;
        } else if (c == ';') {
            if (count < max) {
                out[count++] = have ? val : 0;
            }
            val = 0;
            have = false;
        }
    }
    return count;
}

static void console_reset(console_t *cs);

static void ansi_execute(console_t *cs)
{
    cs->esc_buf[cs->esc_len] = '\0';
    char final = cs->esc_len ? cs->esc_buf[cs->esc_len - 1] : '\0';
    int params[4];
    int n = ansi_params(cs->esc_buf, cs->esc_len > 0 ? cs->esc_len - 1 : 0, params, 4);

    switch (final) {
    case 'm':
        ansi_sgr(cs, cs->esc_buf);
        break;
    case 'J': {
        int mode = (n > 0) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            console_reset(cs);
        } else if (mode == 1) {
            for (size_t y = 0; y < cs->row; ++y) {
                for (size_t x = 0; x < vga_cols; ++x) {
                    set_cell(cs, y, x, ' ', cs->color);
                }
            }
            for (size_t x = 0; x <= cs->col && x < vga_cols; ++x) {
                set_cell(cs, cs->row, x, ' ', cs->color);
            }
        } else {
            for (size_t x = cs->col; x < vga_cols; ++x) {
                set_cell(cs, cs->row, x, ' ', cs->color);
            }
            for (size_t y = cs->row + 1; y < vga_rows; ++y) {
                for (size_t x = 0; x < vga_cols; ++x) {
                    set_cell(cs, y, x, ' ', cs->color);
                }
            }
        }
        break;
    }
    case 'K': {
        int mode = (n > 0) ? params[0] : 0;
        if (mode == 1) {
            for (size_t x = 0; x <= cs->col && x < vga_cols; ++x) {
                set_cell(cs, cs->row, x, ' ', cs->color);
            }
        } else if (mode == 2) {
            for (size_t x = 0; x < vga_cols; ++x) {
                set_cell(cs, cs->row, x, ' ', cs->color);
            }
        } else {
            erase_current_line(cs);
        }
        break;
    }
    case 'H':
    case 'f': {
        int r = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int c = (n > 1 && params[1] > 0) ? params[1] - 1 : 0;
        cs->row = (size_t)(r >= (int)vga_rows ? (int)vga_rows - 1 : r);
        cs->col = (size_t)(c >= (int)vga_cols ? (int)vga_cols - 1 : c);
        break;
    }
    case 'G': {
        int c = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        cs->col = (size_t)(c >= (int)vga_cols ? (int)vga_cols - 1 : c);
        break;
    }
    case 'r': {
        if (n >= 2 && params[0] > 0 && params[1] > 0) {
            cs->scroll_top = params[0] - 1;
            cs->scroll_bottom = params[1] - 1;
            if (cs->scroll_bottom >= (int)vga_rows) {
                cs->scroll_bottom = (int)vga_rows - 1;
            }
            if (cs->scroll_top > cs->scroll_bottom) {
                cs->scroll_top = cs->scroll_bottom;
            }
        } else {
            cs->scroll_top = 0;
            cs->scroll_bottom = vga_rows - 1;
        }
        cs->row = 0;
        cs->col = 0;
        break;
    }
    case 'L':
        insert_lines(cs, (n > 0 && params[0] > 0) ? params[0] : 1);
        break;
    case 'M':
        delete_lines(cs, (n > 0 && params[0] > 0) ? params[0] : 1);
        break;
    default:
        break;
    }
    cs->esc_state = 0;
    cs->esc_len = 0;
    update_cursor(cs);
}

/* Sizes the (hardware-wide) console grid to fill whatever framebuffer mode
 * was actually granted, divided by the current font cell size. Falls back
 * to legacy 80x25 VGA text mode when no usable framebuffer was found. */
static void vga_compute_geometry(void)
{
    const fb_info_t *info = fb_get_info();
    uint32_t cw = (uint32_t)font_cell_w();
    uint32_t ch = (uint32_t)font_cell_h();
    use_fb = info->present && info->width >= cw && info->height >= ch;
    if (use_fb) {
        size_t cols = info->width / cw;
        size_t rows = info->height / ch;
        if (cols > MAX_COLS) {
            cols = MAX_COLS;
        }
        if (rows > MAX_ROWS) {
            rows = MAX_ROWS;
        }
        vga_cols = cols;
        vga_rows = rows;
        origin_x = (info->width - (uint32_t)vga_cols * cw) / 2;
        origin_y = (info->height - (uint32_t)vga_rows * ch) / 2;
        fb_fill_rect(0, 0, info->width, info->height, 0x000000);
    } else {
        vga_cols = TEXT_MODE_COLS;
        vga_rows = TEXT_MODE_ROWS;
        origin_x = 0;
        origin_y = 0;
    }
}

/* Blanks the whole grid for one console and, if it's g_active, repaints
 * real hardware in one bulk pass (see the vga_clear() comment history: a
 * screen clear has no per-cell content to preserve, so there's nothing to
 * gain from the per-glyph set_cell() path here). Row/col/cursor/scroll
 * region/esc-parser state resets too — this is also the full "fresh
 * console" initializer kernel/vt.c uses for VT2..VT_COUNT. */
static void console_reset(console_t *cs)
{
    if (cs == g_active) {
        serial_clear();
    }
    for (size_t y = 0; y < vga_rows; ++y) {
        memset(cs->cell_char[y], ' ', vga_cols);
        memset(cs->cell_attr[y], (int)cs->color, vga_cols);
    }
    if (cs == g_active) {
        if (use_fb) {
            uint32_t cw = (uint32_t)font_cell_w();
            uint32_t chh = (uint32_t)font_cell_h();
            fb_fill_rect(origin_x, origin_y, (uint32_t)vga_cols * cw, (uint32_t)vga_rows * chh,
                         vga_rgb_palette[(cs->color >> 4) & 0xF]);
        } else {
            for (size_t y = 0; y < vga_rows; ++y) {
                for (size_t x = 0; x < vga_cols; ++x) {
                    vga_memory[y * vga_cols + x] = vga_entry(' ', cs->color);
                }
            }
        }
    }
    cs->row = 0;
    cs->col = 0;
    cs->esc_state = 0;
    cs->esc_len = 0;
    cs->scroll_top = 0;
    cs->scroll_bottom = vga_rows - 1;
    cs->cursor_valid = false;
    update_cursor(cs);
}

void vga_console_reset(console_t *cs)
{
    console_reset(cs);
}

/* Full repaint of every visible cell from cs's own cell_char/cell_attr —
 * used by kernel/vt.c's vt_switch() once cs has just become g_active, so
 * the newly foregrounded VT shows its actual saved content instead of
 * whatever was left over from the console that owned hardware a moment
 * ago. Deliberately bypasses set_cell()'s "did it change" check (force_draw,
 * same reasoning as insert_lines()/delete_lines()). */
void vga_console_repaint(console_t *cs)
{
    if (cs != g_active) {
        return;
    }
    if (use_fb) {
        fb_fill_rect(origin_x, origin_y, (uint32_t)vga_cols * (uint32_t)font_cell_w(),
                     (uint32_t)vga_rows * (uint32_t)font_cell_h(), 0x000000);
    }
    for (size_t y = 0; y < vga_rows; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            force_draw(cs, y, x);
        }
    }
    cs->cursor_valid = false;
    update_cursor(cs);
}

/* Switches which console owns real hardware (framebuffer/text-mode memory,
 * hardware cursor, serial mirroring) and repaints it in full. The actual
 * "which VT is active" policy (which n, keyboard routing, ...) lives in
 * kernel/vt.c — this is purely the rendering half of a VT switch. */
void vga_bind_active(console_t *cs)
{
    if (!cs || cs == g_active) {
        return;
    }
    g_active = cs;
    vga_console_repaint(cs);
}

void vga_init(void)
{
    vga_compute_geometry();
    console_t *cs = &g_consoles[0];
    g_active = cs;
    cs->color = vga_default_color();
    console_reset(cs);
}

/* Re-derives the (hardware-wide) grid for a new font pixel size and repaints
 * g_active. Only meaningful with an active framebuffer. A backgrounded
 * console's own buffer is left at its old row/col bounds — see the `font`
 * shell command (shell/builtins.c) and drivers.md for this known limitation
 * of resizing while multiple VTs hold independent buffers. */
bool vga_apply_font_scale(int scale)
{
    if (!use_fb) {
        return false;
    }
    if (!font_set_scale(scale)) {
        return false;
    }
    vga_compute_geometry();
    vga_clear();
    return true;
}

void vga_clear(void)
{
    console_reset(g_active);
}

void vga_putc_vt(console_t *cs, char c)
{
    if (cs == g_active) {
        serial_putc(c);
    }
    if (cs->esc_state == 1) {
        if (c == '[') {
            cs->esc_state = 2;
            cs->esc_len = 0;
            return;
        }
        cs->esc_state = 0;
    } else if (cs->esc_state == 2) {
        if (cs->esc_len + 1 < sizeof(cs->esc_buf)) {
            cs->esc_buf[cs->esc_len++] = c;
        }
        if ((c >= '@' && c <= '~') || cs->esc_len + 1 >= sizeof(cs->esc_buf)) {
            ansi_execute(cs);
        }
        return;
    }

    if (c == '\033') {
        cs->esc_state = 1;
        return;
    }
    if (cs->sb_view != 0) {
        /* Real terminal behavior: any new output snaps the view back to
         * live before it's drawn, so scrolled-back history never gets
         * silently overwritten by force_draw()'s hardware writes. */
        cs->sb_view = 0;
        vga_console_repaint(cs);
    }
    if (c == '\n') {
        newline(cs);
    } else if (c == '\r') {
        cs->col = 0;
    } else if (c == '\t') {
        do {
            vga_putc_vt(cs, ' ');
        } while (cs->col % 4);
    } else if (c == '\b') {
        if (cs->col > 0) {
            cs->col--;
        } else if (cs->row > 0) {
            cs->row--;
            cs->col = vga_cols - 1;
        }
        set_cell(cs, cs->row, cs->col, ' ', cs->color);
    } else {
        set_cell(cs, cs->row, cs->col, c, cs->color);
        if (++cs->col == vga_cols) {
            newline(cs);
        }
    }
    update_cursor(cs);
}

void vga_write_len_vt(console_t *cs, const char *str, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        vga_putc_vt(cs, str[i]);
    }
}

void vga_putc(char c)
{
    vga_putc_vt(g_active, c);
}

void vga_write(const char *str)
{
    vga_write_len_vt(g_active, str, strlen(str));
}

void vga_write_len(const char *str, size_t len)
{
    vga_write_len_vt(g_active, str, len);
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    g_active->color = make_color(fg, bg);
}

uint8_t vga_color(void)
{
    return g_active->color;
}

void vga_get_cursor(size_t *out_row, size_t *out_col)
{
    if (out_row) {
        *out_row = g_active->row;
    }
    if (out_col) {
        *out_col = g_active->col;
    }
}

void vga_get_size(size_t *out_rows, size_t *out_cols)
{
    if (out_rows) {
        *out_rows = vga_rows;
    }
    if (out_cols) {
        *out_cols = vga_cols;
    }
}

void vga_goto(size_t new_row, size_t new_col)
{
    g_active->row = new_row >= vga_rows ? vga_rows - 1 : new_row;
    g_active->col = new_col >= vga_cols ? vga_cols - 1 : new_col;
    serial_move_cursor((unsigned)g_active->row, (unsigned)g_active->col);
}

void vga_erase_eol(void)
{
    console_t *cs = g_active;
    serial_erase_line();
    for (size_t x = cs->col; x < vga_cols; ++x) {
        set_cell(cs, cs->row, x, ' ', cs->color);
    }
}

void vga_set_cursor(size_t new_row, size_t new_col)
{
    console_t *cs = g_active;
    cs->row = new_row >= vga_rows ? vga_rows - 1 : new_row;
    cs->col = new_col >= vga_cols ? vga_cols - 1 : new_col;
    serial_move_cursor((unsigned)cs->row, (unsigned)cs->col);
    update_cursor(cs);
}

void vga_move_cursor_rel(int drow, int dcol)
{
    console_t *cs = g_active;
    int nr = (int)cs->row + drow;
    int nc = (int)cs->col + dcol;
    if (nr < 0) nr = 0;
    if (nc < 0) nc = 0;
    if (nr >= (int)vga_rows) nr = (int)vga_rows - 1;
    if (nc >= (int)vga_cols) nc = (int)vga_cols - 1;
    cs->row = (size_t)nr;
    cs->col = (size_t)nc;
    update_cursor(cs);
}

void vga_status_bar(const char *left, const char *right)
{
    console_t *cs = g_active;
    size_t saved_row = cs->row;
    size_t saved_col = cs->col;
    uint8_t saved_color = cs->color;
    cs->row = vga_rows - 1;
    cs->col = 0;
    serial_move_cursor((unsigned)cs->row, (unsigned)cs->col);
    cs->color = make_color(VGA_BLACK, VGA_LIGHT_GREY);
    for (size_t i = 0; i < vga_cols; ++i) {
        set_cell(cs, cs->row, i, ' ', cs->color);
    }
    vga_write(left ? left : "");
    if (right) {
        size_t rlen = strlen(right);
        if (rlen < vga_cols) {
            cs->row = vga_rows - 1;
            cs->col = vga_cols - rlen;
            serial_move_cursor((unsigned)cs->row, (unsigned)cs->col);
            vga_write(right);
        }
    }
    cs->color = saved_color;
    cs->row = saved_row;
    cs->col = saved_col;
    serial_move_cursor((unsigned)cs->row, (unsigned)cs->col);
    update_cursor(cs);
}

/* Scrollback viewing: shifts the *displayed* rows (never the logical
 * cell_char/cell_attr buffer) `lines` further back into history (positive)
 * or forward toward live (negative), clamped to what's actually available,
 * and repaints g_active from that blended view. delta == 0 with sb_view
 * already 0 is a no-op. Only meaningful for g_active — a backgrounded
 * console has nothing on screen to scroll. */
void vga_console_scroll_view(console_t *cs, int delta)
{
    if (cs != g_active) {
        return;
    }
    long view = (long)cs->sb_view - delta;
    if (view < 0) {
        view = 0;
    }
    if (view > (long)cs->sb_count) {
        view = (long)cs->sb_count;
    }
    cs->sb_view = (size_t)view;

    if (use_fb) {
        fb_fill_rect(origin_x, origin_y, (uint32_t)vga_cols * (uint32_t)font_cell_w(),
                     (uint32_t)vga_rows * (uint32_t)font_cell_h(), 0x000000);
    }
    for (size_t y = 0; y < vga_rows; ++y) {
        /* Row y of the blended view: the last `sb_view` history lines come
         * first (oldest of that window first), then the live grid resumes. */
        long src = (long)y - (long)cs->sb_view;
        const char *text;
        char blank_row[MAX_COLS];
        if (src < 0) {
            long hist_index = (long)cs->sb_count + src; /* how far back from newest */
            size_t ring_slot = (size_t)(((long)cs->sb_next - (long)cs->sb_count + hist_index)
                                         % SCROLLBACK_LINES + SCROLLBACK_LINES) % SCROLLBACK_LINES;
            text = cs->sb_char[ring_slot];
        } else if ((size_t)src < vga_rows) {
            text = cs->cell_char[src];
        } else {
            memset(blank_row, ' ', vga_cols);
            text = blank_row;
        }
        for (size_t x = 0; x < vga_cols; ++x) {
            char ch = x < MAX_COLS ? text[x] : ' ';
            if (ch == '\0') {
                ch = ' ';
            }
            draw_cell_ex(cs, y, x, ch, cs->color, false);
        }
    }
}

size_t vga_console_scrollback_count(const console_t *cs)
{
    return cs->sb_count;
}
