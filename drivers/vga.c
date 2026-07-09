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

static const uint32_t vga_rgb_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static volatile uint16_t *const vga_memory = (uint16_t *)0xB8000;
static char cell_char[MAX_ROWS][MAX_COLS];
static uint8_t cell_attr[MAX_ROWS][MAX_COLS];
static bool use_fb;
/* Pixel origin of the text grid within the framebuffer. Bootloaders treat
 * the multiboot2 framebuffer tag as a preference, not a guarantee, and the
 * grid itself is sized to fill whatever mode was actually granted (see
 * vga_compute_geometry()), so there's usually a small leftover margin from
 * rounding down to a whole number of cells; center it. */
static uint32_t origin_x;
static uint32_t origin_y;
/* Framebuffer-backed grid dimensions, computed at vga_init() time (and
 * recomputed by vga_apply_font_scale()) from the detected framebuffer
 * resolution and the current font cell size — see vga_compute_geometry().
 * In legacy text mode these are always exactly TEXT_MODE_COLS/ROWS. */
static size_t vga_cols = TEXT_MODE_COLS;
static size_t vga_rows = TEXT_MODE_ROWS;
static size_t row;
static size_t col;
static uint8_t color;
static int esc_state;
static char esc_buf[16];
static size_t esc_len;
/* DECSTBM scroll region, 0-indexed and inclusive on both ends. Neatvi (see
 * user/vi/term.c's term_window()) keeps the bottom line as a fixed status
 * bar by confining the scrolling area to everything above it — newline()/
 * scroll() must only ever shift rows in [scroll_top, scroll_bottom], never
 * the whole 25-row screen, or that status line would scroll away with the
 * rest of the buffer on every line feed. */
static int scroll_top;
static int scroll_bottom;

/* Tracks where the software cursor overlay is currently drawn on the
 * framebuffer, so it can be erased (by redrawing the true cell contents)
 * before being redrawn at its new position. Unused in legacy text mode,
 * which relies on the hardware cursor instead. */
static size_t cur_row;
static size_t cur_col;
static bool cursor_valid;

/* ---- Perf instrumentation -------------------------------------------------
 * Cheap always-on counters for the metrics this console's redraw path was
 * suspected of getting wrong on real hardware: glyphs actually rendered,
 * bytes actually moved, and cycles spent in each. Cost per call is one
 * rdtsc pair and a handful of adds — negligible next to the work being
 * measured. Dumped to raw serial (not through vga_putc/serial_putc, which
 * would recursively feed the very console activity being measured) once per
 * scroll so a real-hardware serial log shows exactly what each newline
 * cost, without needing a UI to read counters back out. */
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

/* Renders one glyph at scale font_get_scale(): each base glyph pixel becomes
 * an SxS block of framebuffer pixels (nearest-neighbor upscale), so the
 * `font` command can grow console text without a new rasterized bitmap. */
static void draw_cell_ex(size_t r, size_t c, char ch, uint8_t attr, bool invert)
{
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

static void draw_cell(size_t r, size_t c)
{
    draw_cell_ex(r, c, cell_char[r][c], cell_attr[r][c], false);
}

/* Unconditionally repaints (r, c) from the current cell_char/cell_attr
 * contents, bypassing set_cell()'s change check below. Used where the
 * backing array was already updated in bulk (memmove/memcpy) before the
 * pixels were — set_cell() would see "no change" and wrongly skip the
 * redraw in that case. */
static void force_draw(size_t r, size_t c)
{
    if (use_fb) {
        draw_cell(r, c);
    } else {
        vga_memory[r * vga_cols + c] = vga_entry(cell_char[r][c], cell_attr[r][c]);
    }
}

/* Skips the redraw entirely when neither the character nor its attribute
 * actually changed — the single highest-leverage fix for avoiding
 * unnecessary redraw work, since every higher-level redraw path (shell line
 * editing, the vi/vim builtin, neatvi) already works by reprinting content
 * that's mostly unchanged from one frame to the next (e.g. "\r\033[K" +
 * prompt + line on every keystroke) rather than diffing itself. */
static void set_cell(size_t r, size_t c, char ch, uint8_t attr)
{
    if (cell_char[r][c] == ch && cell_attr[r][c] == attr) {
        return;
    }
    cell_char[r][c] = ch;
    cell_attr[r][c] = attr;
    force_draw(r, c);
}

static void cursor_restore(void)
{
    if (use_fb && cursor_valid) {
        draw_cell(cur_row, cur_col);
    }
}

static void cursor_draw(void)
{
    if (use_fb) {
        draw_cell_ex(row, col, cell_char[row][col], cell_attr[row][col], true);
        cur_row = row;
        cur_col = col;
        cursor_valid = true;
    }
}

static void update_cursor(void)
{
    if (use_fb) {
        cursor_restore();
        cursor_draw();
        return;
    }
    uint16_t pos = (uint16_t)(row * vga_cols + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

/* Shifts [scroll_top, scroll_bottom] up by one line, blanking the newly
 * exposed bottom line of the region. Rows outside the region (e.g. a fixed
 * status line below scroll_bottom) are left untouched. With the default
 * full-screen region (0, vga_rows-1) this reproduces the previous
 * whole-screen-scroll behavior exactly.
 *
 * cell_char/cell_attr rows are MAX_COLS apart, not vga_cols apart (see their
 * declaration), so unlike the fixed-80-column original this can't shift
 * every affected row with one bulk memmove spanning multiple rows — each
 * row is moved individually instead. Still O(rows), not O(rows*cols): the
 * expensive part (repainting pixels) stays a single fb_scroll_up() blit. */
static void scroll(void)
{
    int top = scroll_top;
    int bot = scroll_bottom;
    if (bot <= top) {
        return;
    }
    uint64_t t0 = rdtsc();
    uint32_t bytes = 0;
    for (int y = top; y < bot; ++y) {
        memmove(cell_char[y], cell_char[y + 1], vga_cols);
        memmove(cell_attr[y], cell_attr[y + 1], vga_cols);
    }
    bytes += (uint32_t)((bot - top) * (long)vga_cols * 2);
    for (size_t x = 0; x < vga_cols; ++x) {
        cell_char[bot][x] = ' ';
        cell_attr[bot][x] = color;
    }
    if (use_fb) {
        uint32_t cw = (uint32_t)font_cell_w();
        uint32_t ch = (uint32_t)font_cell_h();
        fb_scroll_up(origin_x, origin_y + (uint32_t)top * ch,
                     (uint32_t)vga_cols * cw, (uint32_t)(bot - top + 1) * ch,
                     ch, vga_rgb_palette[(color >> 4) & 0xF]);
        bytes += (uint32_t)(bot - top) * ch * (uint32_t)vga_cols * cw * (fb_get_info()->bpp / 8);
        if (cursor_valid && (int)cur_row >= top && (int)cur_row <= bot) {
            if ((int)cur_row == top) {
                cursor_valid = false;
            } else {
                cur_row--;
            }
        }
    } else {
        for (int y = top + 1; y <= bot; ++y) {
            for (size_t x = 0; x < vga_cols; ++x) {
                vga_memory[(size_t)(y - 1) * vga_cols + x] = vga_memory[(size_t)y * vga_cols + x];
            }
        }
        for (size_t x = 0; x < vga_cols; ++x) {
            vga_memory[(size_t)bot * vga_cols + x] = vga_entry(' ', color);
        }
        bytes += (uint32_t)((bot - top) * (long)vga_cols * 2);
    }
    uint32_t cycles = (uint32_t)(rdtsc() - t0);
    /* Every scroll, not batched — newline is exactly the operation real
     * hardware was visibly slow on, so each one gets its own serial line
     * rather than being averaged away. */
    perf_scroll_calls++;
    perf_report("scroll", (uint32_t)(bot - top + 1), bytes, cycles);
}

/* Inserts/deletes |n| blank lines at the cursor row, shifting the rest of
 * the scroll region down/up (VT100 IL/DL — CSI n L / CSI n M) — used by
 * term_room() in user/vi/term.c during partial-screen scroll redraws.
 * Implemented as a full redraw of the affected rows via force_draw() rather
 * than scroll()'s memmove+fb_scroll_up fast path, since it's not the hot
 * path and works identically for both the text-mode and framebuffer
 * backends without a "scroll down" primitive.
 *
 * force_draw(), not set_cell(): the backing array is already updated to its
 * final state by the memcpy/blank loops above by the time the pixel-draw
 * loop runs, so set_cell()'s "did the value actually change" check would
 * always see "no" here and wrongly skip repainting every shifted row. */
static void insert_lines(int n)
{
    int top = (int)row;
    int bot = scroll_bottom;
    if (top > bot || n <= 0) {
        return;
    }
    if (n > bot - top + 1) {
        n = bot - top + 1;
    }
    for (int y = bot; y >= top + n; --y) {
        memcpy(cell_char[y], cell_char[y - n], vga_cols);
        memcpy(cell_attr[y], cell_attr[y - n], vga_cols);
    }
    for (int y = top; y < top + n; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            cell_char[y][x] = ' ';
            cell_attr[y][x] = color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            force_draw((size_t)y, x);
        }
    }
}

static void delete_lines(int n)
{
    int top = (int)row;
    int bot = scroll_bottom;
    if (top > bot || n <= 0) {
        return;
    }
    if (n > bot - top + 1) {
        n = bot - top + 1;
    }
    for (int y = top; y <= bot - n; ++y) {
        memcpy(cell_char[y], cell_char[y + n], vga_cols);
        memcpy(cell_attr[y], cell_attr[y + n], vga_cols);
    }
    for (int y = bot - n + 1; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            cell_char[y][x] = ' ';
            cell_attr[y][x] = color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < vga_cols; ++x) {
            force_draw((size_t)y, x);
        }
    }
}

static void newline(void)
{
    col = 0;
    if ((int)row == scroll_bottom) {
        scroll();
    } else if (row + 1 < vga_rows) {
        row++;
    }
}

/* CSI K (EL, "Erase in Line") with no parameter — real ANSI's default
 * parameter 0 means "erase from the cursor to the end of the line", not
 * the whole line: this used to start from column 0 and reset col to 0
 * unconditionally, silently wiping out whatever had just been written
 * earlier on the same line before the cursor's current position. Neatvi's
 * redraw pattern (see user/vi/term.c) is exactly "position, write the
 * line's real content, then \33[K to blank any leftover trailing chars
 * from a previously-longer line at that row" — with the old whole-line
 * erase, that \33[K deleted the content it had just drawn, on every row,
 * every redraw. Matches vga_erase_eol() below, which already had this
 * right; cursor position is left unchanged, matching real EL0. */
static void erase_current_line(void)
{
    serial_erase_line();
    for (size_t x = col; x < vga_cols; ++x) {
        set_cell(row, x, ' ', color);
    }
}

/* ANSI SGR color numbers (black,red,green,yellow,blue,magenta,cyan,white,
 * i.e. 30-37/90-97/40-47) count in a different order than the VGA/CGA
 * palette this driver indexes directly (vga_rgb_palette[] above: black,
 * blue,green,cyan,red,magenta,brown,grey) — red and blue are swapped, and
 * so are yellow and cyan. Neatvi's term_seqattr() (user/vi/term.c) emits
 * plain ANSI numbers, so without this table its blue ("34") rendered as
 * VGA index 4 (red) and its red ("31") rendered as VGA index 1 (blue). */
static const uint8_t ansi_to_vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

static void ansi_sgr(const char *seq)
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
                color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
            } else if (value >= 30 && value <= 37) {
                color = make_color(ansi_to_vga[value - 30], color >> 4);
            } else if (value >= 90 && value <= 97) {
                color = make_color((uint8_t)(ansi_to_vga[value - 90] + 8), color >> 4);
            } else if (value >= 40 && value <= 47) {
                color = make_color(color & 0x0F, ansi_to_vga[value - 40]);
            }
            value = 0;
            have_value = false;
        }
        if (c == 'm' || c == '\0') {
            break;
        }
    }
}

/* Parses up to `max` semicolon-separated decimal parameters from seq[0..n),
 * defaulting an empty/omitted parameter to 0 (the caller decides what that
 * means — usually "1" for a 1-indexed position, or "act as if absent"). */
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

static void ansi_execute(void)
{
    esc_buf[esc_len] = '\0';
    char final = esc_len ? esc_buf[esc_len - 1] : '\0';
    int params[4];
    int n = ansi_params(esc_buf, esc_len > 0 ? esc_len - 1 : 0, params, 4);

    switch (final) {
    case 'm':
        ansi_sgr(esc_buf);
        break;
    case 'J': {
        /* ED (Erase in Display). Unlike the old unconditional vga_clear(),
         * this now distinguishes the three real ANSI modes and — except for
         * mode 2/3, a genuine full-screen clear — erases only the requested
         * sub-region without resetting cursor position or the scroll
         * region, matching real terminal behavior. */
        int mode = (n > 0) ? params[0] : 0;
        if (mode == 2 || mode == 3) {
            vga_clear();
        } else if (mode == 1) {
            for (size_t y = 0; y < row; ++y) {
                for (size_t x = 0; x < vga_cols; ++x) {
                    set_cell(y, x, ' ', color);
                }
            }
            for (size_t x = 0; x <= col && x < vga_cols; ++x) {
                set_cell(row, x, ' ', color);
            }
        } else {
            for (size_t x = col; x < vga_cols; ++x) {
                set_cell(row, x, ' ', color);
            }
            for (size_t y = row + 1; y < vga_rows; ++y) {
                for (size_t x = 0; x < vga_cols; ++x) {
                    set_cell(y, x, ' ', color);
                }
            }
        }
        break;
    }
    case 'K': {
        /* EL (Erase in Line): 0 (default) = cursor to EOL, 1 = start to
         * cursor, 2 = whole line. Cursor position never moves. */
        int mode = (n > 0) ? params[0] : 0;
        if (mode == 1) {
            for (size_t x = 0; x <= col && x < vga_cols; ++x) {
                set_cell(row, x, ' ', color);
            }
        } else if (mode == 2) {
            for (size_t x = 0; x < vga_cols; ++x) {
                set_cell(row, x, ' ', color);
            }
        } else {
            erase_current_line();
        }
        break;
    }
    case 'H':
    case 'f': {
        /* CUP: 1-indexed row;col, both defaulting to 1 (i.e. row/col 0). */
        int r = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int c = (n > 1 && params[1] > 0) ? params[1] - 1 : 0;
        row = (size_t)(r >= (int)vga_rows ? (int)vga_rows - 1 : r);
        col = (size_t)(c >= (int)vga_cols ? (int)vga_cols - 1 : c);
        break;
    }
    case 'G': {
        /* CHA: 1-indexed column only, same row. */
        int c = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        col = (size_t)(c >= (int)vga_cols ? (int)vga_cols - 1 : c);
        break;
    }
    case 'r': {
        /* DECSTBM: 1-indexed top;bottom, or reset to the full screen when
         * either parameter is missing/zero (also how a bare "\33[r" — no
         * parameters at all — parses: n == 1, params[0] == 0). */
        if (n >= 2 && params[0] > 0 && params[1] > 0) {
            scroll_top = params[0] - 1;
            scroll_bottom = params[1] - 1;
            if (scroll_bottom >= (int)vga_rows) {
                scroll_bottom = (int)vga_rows - 1;
            }
            if (scroll_top > scroll_bottom) {
                scroll_top = scroll_bottom;
            }
        } else {
            scroll_top = 0;
            scroll_bottom = vga_rows - 1;
        }
        /* Real terminals home the cursor after changing the scroll region. */
        row = 0;
        col = 0;
        break;
    }
    case 'L':
        insert_lines((n > 0 && params[0] > 0) ? params[0] : 1);
        break;
    case 'M':
        delete_lines((n > 0 && params[0] > 0) ? params[0] : 1);
        break;
    default:
        break;
    }
    esc_state = 0;
    esc_len = 0;
    update_cursor();
}

/* Sizes the console grid to fill whatever framebuffer mode was actually
 * granted (divided by the current font cell size), instead of assuming a
 * fixed 80x25 — bootloaders treat the multiboot2 framebuffer tag as a
 * preference, not a guarantee, and real hardware hands back whatever native
 * mode it defaults to, which is routinely far larger than 80x25 (see
 * docs/boot.md). Falls back to legacy 80x25 VGA text mode when no usable
 * framebuffer was found, or when it's too small to even fit one cell.
 * Callable more than once: vga_apply_font_scale() re-runs this after
 * changing the font's pixel size so the grid is re-derived for the new cell
 * size, without needing a full vga_init(). */
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

void vga_init(void)
{
    vga_compute_geometry();
    color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
    scroll_top = 0;
    scroll_bottom = vga_rows - 1;
    cursor_valid = false;
    vga_clear();
}

/* Re-derives the grid for a new font pixel size and repaints — see the
 * `font` shell command (shell/builtins.c). Only meaningful with an active
 * framebuffer; legacy 80x25 text mode has no adjustable glyph size (real
 * VGA text-mode hardware generates its own on-screen glyphs from the
 * character code, this driver never rasterizes into it). Returns false if
 * the console is in legacy text mode or scale is out of range, in which
 * case nothing changes. */
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

/* Blanks the whole grid. Framebuffer path used to loop set_cell() over every
 * cell, which — since the incoming content is virtually never already
 * blank — meant force_draw()'ing (i.e. fully rasterizing) a space glyph at
 * every one of vga_rows*vga_cols cells: thousands of individual glyph
 * renders just to paint a solid color. A screen clear has no per-cell
 * content to preserve, so there's nothing to gain from going through the
 * per-glyph path at all: one fb_fill_rect() over the whole grid area does
 * the same pixels in one bulk pass (see fb_fill_rect() in
 * drivers/framebuffer.c), and the backing cell_char/cell_attr arrays are
 * reset with memset() per row instead of one assignment per cell. */
void vga_clear(void)
{
    serial_clear();
    if (use_fb) {
        for (size_t y = 0; y < vga_rows; ++y) {
            memset(cell_char[y], ' ', vga_cols);
            memset(cell_attr[y], (int)color, vga_cols);
        }
        uint32_t cw = (uint32_t)font_cell_w();
        uint32_t ch = (uint32_t)font_cell_h();
        fb_fill_rect(origin_x, origin_y, (uint32_t)vga_cols * cw, (uint32_t)vga_rows * ch,
                     vga_rgb_palette[(color >> 4) & 0xF]);
    } else {
        for (size_t y = 0; y < vga_rows; ++y) {
            memset(cell_char[y], ' ', vga_cols);
            memset(cell_attr[y], (int)color, vga_cols);
            for (size_t x = 0; x < vga_cols; ++x) {
                vga_memory[y * vga_cols + x] = vga_entry(' ', color);
            }
        }
    }
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
    scroll_top = 0;
    scroll_bottom = vga_rows - 1;
    cursor_valid = false;
    update_cursor();
}

void vga_putc(char c)
{
    serial_putc(c);
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_len = 0;
            return;
        }
        esc_state = 0;
    } else if (esc_state == 2) {
        if (esc_len + 1 < sizeof(esc_buf)) {
            esc_buf[esc_len++] = c;
        }
        if ((c >= '@' && c <= '~') || esc_len + 1 >= sizeof(esc_buf)) {
            ansi_execute();
        }
        return;
    }

    if (c == '\033') {
        esc_state = 1;
        return;
    }
    if (c == '\n') {
        newline();
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\t') {
        do {
            vga_putc(' ');
        } while (col % 4);
    } else if (c == '\b') {
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = vga_cols - 1;
        }
        set_cell(row, col, ' ', color);
    } else {
        set_cell(row, col, c, color);
        if (++col == vga_cols) {
            newline();
        }
    }
    update_cursor();
}

void vga_write(const char *str)
{
    vga_write_len(str, strlen(str));
}

void vga_write_len(const char *str, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        vga_putc(str[i]);
    }
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    color = make_color(fg, bg);
}

uint8_t vga_color(void)
{
    return color;
}

void vga_get_cursor(size_t *out_row, size_t *out_col)
{
    if (out_row) {
        *out_row = row;
    }
    if (out_col) {
        *out_col = col;
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

/* Move write position + serial cursor WITHOUT touching the visible cursor.
   Use during bulk screen redraws to avoid cursor flicker on row 0. */
void vga_goto(size_t new_row, size_t new_col)
{
    row = new_row >= vga_rows ? vga_rows - 1 : new_row;
    col = new_col >= vga_cols ? vga_cols - 1 : new_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
}

/* Clear from current column to end of line: \033[K on serial, blanked cells
   on screen. */
void vga_erase_eol(void)
{
    serial_erase_line();
    for (size_t x = col; x < vga_cols; ++x) {
        set_cell(row, x, ' ', color);
    }
}

void vga_set_cursor(size_t new_row, size_t new_col)
{
    row = new_row >= vga_rows ? vga_rows - 1 : new_row;
    col = new_col >= vga_cols ? vga_cols - 1 : new_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
    update_cursor();
}

void vga_move_cursor_rel(int drow, int dcol)
{
    int nr = (int)row + drow;
    int nc = (int)col + dcol;
    if (nr < 0) nr = 0;
    if (nc < 0) nc = 0;
    if (nr >= (int)vga_rows) nr = (int)vga_rows - 1;
    if (nc >= (int)vga_cols) nc = (int)vga_cols - 1;
    row = (size_t)nr;
    col = (size_t)nc;
    update_cursor();
}

void vga_status_bar(const char *left, const char *right)
{
    size_t saved_row = row;
    size_t saved_col = col;
    uint8_t saved_color = color;
    row = vga_rows - 1;
    col = 0;
    serial_move_cursor((unsigned)row, (unsigned)col);
    color = make_color(VGA_BLACK, VGA_LIGHT_GREY);
    for (size_t i = 0; i < vga_cols; ++i) {
        set_cell(row, i, ' ', color);
    }
    vga_write(left ? left : "");
    if (right) {
        size_t rlen = strlen(right);
        if (rlen < vga_cols) {
            row = vga_rows - 1;
            col = vga_cols - rlen;
            serial_move_cursor((unsigned)row, (unsigned)col);
            vga_write(right);
        }
    }
    color = saved_color;
    row = saved_row;
    col = saved_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
    update_cursor();
}
