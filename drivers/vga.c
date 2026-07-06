#include <pureunix/framebuffer.h>
#include <pureunix/font.h>
#include <pureunix/io.h>
#include <pureunix/serial.h>
#include <pureunix/string.h>
#include <pureunix/vga.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static const uint32_t vga_rgb_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static volatile uint16_t *const vga_memory = (uint16_t *)0xB8000;
static char cell_char[VGA_HEIGHT][VGA_WIDTH];
static uint8_t cell_attr[VGA_HEIGHT][VGA_WIDTH];
static bool use_fb;
/* Pixel origin of the 80x25 text grid within the framebuffer. Bootloaders
 * treat the multiboot2 framebuffer tag as a preference, not a guarantee, so
 * the granted mode is usually larger than the grid; center it. */
static uint32_t origin_x;
static uint32_t origin_y;
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

static uint16_t vga_entry(char c, uint8_t entry_color)
{
    return (uint16_t)c | ((uint16_t)entry_color << 8);
}

static uint8_t make_color(uint8_t fg, uint8_t bg)
{
    return fg | (bg << 4);
}

static void draw_cell_ex(size_t r, size_t c, char ch, uint8_t attr, bool invert)
{
    uint32_t fg = vga_rgb_palette[invert ? (attr >> 4) & 0xF : attr & 0xF];
    uint32_t bg = vga_rgb_palette[invert ? attr & 0xF : (attr >> 4) & 0xF];
    const uint8_t *glyph = font_glyph(ch);
    uint32_t px0 = origin_x + (uint32_t)(c * FONT_CELL_W);
    uint32_t py0 = origin_y + (uint32_t)(r * FONT_CELL_H);
    for (uint32_t y = 0; y < FONT_CELL_H; ++y) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_CELL_W; ++x) {
            fb_put_pixel(px0 + x, py0 + y, (bits & (0x80 >> x)) ? fg : bg);
        }
    }
}

static void draw_cell(size_t r, size_t c)
{
    draw_cell_ex(r, c, cell_char[r][c], cell_attr[r][c], false);
}

static void set_cell(size_t r, size_t c, char ch, uint8_t attr)
{
    cell_char[r][c] = ch;
    cell_attr[r][c] = attr;
    if (use_fb) {
        draw_cell(r, c);
    } else {
        vga_memory[r * VGA_WIDTH + c] = vga_entry(ch, attr);
    }
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
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

/* Shifts [scroll_top, scroll_bottom] up by one line, blanking the newly
 * exposed bottom line of the region. Rows outside the region (e.g. a fixed
 * status line below scroll_bottom) are left untouched. With the default
 * full-screen region (0, VGA_HEIGHT-1) this reproduces the previous
 * whole-screen-scroll behavior exactly. */
static void scroll(void)
{
    int top = scroll_top;
    int bot = scroll_bottom;
    if (bot <= top) {
        return;
    }
    memmove(cell_char[top], cell_char[top + 1], (size_t)(bot - top) * VGA_WIDTH);
    memmove(cell_attr[top], cell_attr[top + 1], (size_t)(bot - top) * VGA_WIDTH);
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        cell_char[bot][x] = ' ';
        cell_attr[bot][x] = color;
    }
    if (use_fb) {
        fb_scroll_up(origin_x, origin_y + (uint32_t)top * FONT_CELL_H,
                     VGA_WIDTH * FONT_CELL_W, (uint32_t)(bot - top + 1) * FONT_CELL_H,
                     FONT_CELL_H, vga_rgb_palette[(color >> 4) & 0xF]);
        if (cursor_valid && (int)cur_row >= top && (int)cur_row <= bot) {
            if ((int)cur_row == top) {
                cursor_valid = false;
            } else {
                cur_row--;
            }
        }
    } else {
        for (int y = top + 1; y <= bot; ++y) {
            for (size_t x = 0; x < VGA_WIDTH; ++x) {
                vga_memory[(size_t)(y - 1) * VGA_WIDTH + x] = vga_memory[(size_t)y * VGA_WIDTH + x];
            }
        }
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga_memory[(size_t)bot * VGA_WIDTH + x] = vga_entry(' ', color);
        }
    }
}

/* Inserts/deletes |n| blank lines at the cursor row, shifting the rest of
 * the scroll region down/up (VT100 IL/DL — CSI n L / CSI n M) — used by
 * term_room() in user/vi/term.c during partial-screen scroll redraws.
 * Implemented as a full redraw of the affected rows via set_cell() rather
 * than scroll()'s memmove+fb_scroll_up fast path, since it's not the hot
 * path and works identically for both the text-mode and framebuffer
 * backends without a "scroll down" primitive. */
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
        memcpy(cell_char[y], cell_char[y - n], VGA_WIDTH);
        memcpy(cell_attr[y], cell_attr[y - n], VGA_WIDTH);
    }
    for (int y = top; y < top + n; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            cell_char[y][x] = ' ';
            cell_attr[y][x] = color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            set_cell((size_t)y, x, cell_char[y][x], cell_attr[y][x]);
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
        memcpy(cell_char[y], cell_char[y + n], VGA_WIDTH);
        memcpy(cell_attr[y], cell_attr[y + n], VGA_WIDTH);
    }
    for (int y = bot - n + 1; y <= bot; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            cell_char[y][x] = ' ';
            cell_attr[y][x] = color;
        }
    }
    for (int y = top; y <= bot; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            set_cell((size_t)y, x, cell_char[y][x], cell_attr[y][x]);
        }
    }
}

static void newline(void)
{
    col = 0;
    if ((int)row == scroll_bottom) {
        scroll();
    } else if (row + 1 < VGA_HEIGHT) {
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
    for (size_t x = col; x < VGA_WIDTH; ++x) {
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
    case 'J':
        vga_clear();
        break;
    case 'K':
        erase_current_line();
        break;
    case 'H':
    case 'f': {
        /* CUP: 1-indexed row;col, both defaulting to 1 (i.e. row/col 0). */
        int r = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int c = (n > 1 && params[1] > 0) ? params[1] - 1 : 0;
        row = (size_t)(r >= VGA_HEIGHT ? VGA_HEIGHT - 1 : r);
        col = (size_t)(c >= VGA_WIDTH ? VGA_WIDTH - 1 : c);
        break;
    }
    case 'G': {
        /* CHA: 1-indexed column only, same row. */
        int c = (n > 0 && params[0] > 0) ? params[0] - 1 : 0;
        col = (size_t)(c >= VGA_WIDTH ? VGA_WIDTH - 1 : c);
        break;
    }
    case 'r': {
        /* DECSTBM: 1-indexed top;bottom, or reset to the full screen when
         * either parameter is missing/zero (also how a bare "\33[r" — no
         * parameters at all — parses: n == 1, params[0] == 0). */
        if (n >= 2 && params[0] > 0 && params[1] > 0) {
            scroll_top = params[0] - 1;
            scroll_bottom = params[1] - 1;
            if (scroll_bottom >= VGA_HEIGHT) {
                scroll_bottom = VGA_HEIGHT - 1;
            }
            if (scroll_top > scroll_bottom) {
                scroll_top = scroll_bottom;
            }
        } else {
            scroll_top = 0;
            scroll_bottom = VGA_HEIGHT - 1;
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

void vga_init(void)
{
    const fb_info_t *info = fb_get_info();
    /* Bootloaders treat the framebuffer size request as a preference, not a
     * guarantee, so accept any mode at least as big as our fixed 80x25 grid
     * and center the grid within it. */
    use_fb = info->present && info->width >= VGA_WIDTH * FONT_CELL_W &&
             info->height >= VGA_HEIGHT * FONT_CELL_H;
    if (use_fb) {
        origin_x = (info->width - VGA_WIDTH * FONT_CELL_W) / 2;
        origin_y = (info->height - VGA_HEIGHT * FONT_CELL_H) / 2;
        fb_fill_rect(0, 0, info->width, info->height, 0x000000);
    }
    color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
    scroll_top = 0;
    scroll_bottom = VGA_HEIGHT - 1;
    cursor_valid = false;
    vga_clear();
}

void vga_clear(void)
{
    serial_clear();
    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            set_cell(y, x, ' ', color);
        }
    }
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
    scroll_top = 0;
    scroll_bottom = VGA_HEIGHT - 1;
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
            col = VGA_WIDTH - 1;
        }
        set_cell(row, col, ' ', color);
    } else {
        set_cell(row, col, c, color);
        if (++col == VGA_WIDTH) {
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
        *out_rows = VGA_HEIGHT;
    }
    if (out_cols) {
        *out_cols = VGA_WIDTH;
    }
}

/* Move write position + serial cursor WITHOUT touching the visible cursor.
   Use during bulk screen redraws to avoid cursor flicker on row 0. */
void vga_goto(size_t new_row, size_t new_col)
{
    row = new_row >= VGA_HEIGHT ? VGA_HEIGHT - 1 : new_row;
    col = new_col >= VGA_WIDTH ? VGA_WIDTH - 1 : new_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
}

/* Clear from current column to end of line: \033[K on serial, blanked cells
   on screen. */
void vga_erase_eol(void)
{
    serial_erase_line();
    for (size_t x = col; x < VGA_WIDTH; ++x) {
        set_cell(row, x, ' ', color);
    }
}

void vga_set_cursor(size_t new_row, size_t new_col)
{
    row = new_row >= VGA_HEIGHT ? VGA_HEIGHT - 1 : new_row;
    col = new_col >= VGA_WIDTH ? VGA_WIDTH - 1 : new_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
    update_cursor();
}

void vga_move_cursor_rel(int drow, int dcol)
{
    int nr = (int)row + drow;
    int nc = (int)col + dcol;
    if (nr < 0) nr = 0;
    if (nc < 0) nc = 0;
    if (nr >= VGA_HEIGHT) nr = VGA_HEIGHT - 1;
    if (nc >= VGA_WIDTH) nc = VGA_WIDTH - 1;
    row = (size_t)nr;
    col = (size_t)nc;
    update_cursor();
}

void vga_status_bar(const char *left, const char *right)
{
    size_t saved_row = row;
    size_t saved_col = col;
    uint8_t saved_color = color;
    row = VGA_HEIGHT - 1;
    col = 0;
    serial_move_cursor((unsigned)row, (unsigned)col);
    color = make_color(VGA_BLACK, VGA_LIGHT_GREY);
    for (size_t i = 0; i < VGA_WIDTH; ++i) {
        set_cell(row, i, ' ', color);
    }
    vga_write(left ? left : "");
    if (right) {
        size_t rlen = strlen(right);
        if (rlen < VGA_WIDTH) {
            row = VGA_HEIGHT - 1;
            col = VGA_WIDTH - rlen;
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
