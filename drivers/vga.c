#include <pureunix/io.h>
#include <pureunix/serial.h>
#include <pureunix/string.h>
#include <pureunix/vga.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile uint16_t *const vga_memory = (uint16_t *)0xB8000;
static size_t row;
static size_t col;
static uint8_t color;
static int esc_state;
static char esc_buf[16];
static size_t esc_len;

static uint16_t vga_entry(char c, uint8_t entry_color)
{
    return (uint16_t)c | ((uint16_t)entry_color << 8);
}

static uint8_t make_color(uint8_t fg, uint8_t bg)
{
    return fg | (bg << 4);
}

static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void scroll(void)
{
    if (row < VGA_HEIGHT) {
        return;
    }
    for (size_t y = 1; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga_memory[(y - 1) * VGA_WIDTH + x] = vga_memory[y * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        vga_memory[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color);
    }
    row = VGA_HEIGHT - 1;
}

static void newline(void)
{
    col = 0;
    row++;
    scroll();
}

static void erase_current_line(void)
{
    serial_erase_line();
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        vga_memory[row * VGA_WIDTH + x] = vga_entry(' ', color);
    }
    col = 0;
}

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
                color = make_color((uint8_t)(value - 30), color >> 4);
            } else if (value >= 90 && value <= 97) {
                color = make_color((uint8_t)(value - 90 + 8), color >> 4);
            } else if (value >= 40 && value <= 47) {
                color = make_color(color & 0x0F, (uint8_t)(value - 40));
            }
            value = 0;
            have_value = false;
        }
        if (c == 'm' || c == '\0') {
            break;
        }
    }
}

static void ansi_execute(void)
{
    esc_buf[esc_len] = '\0';
    char final = esc_len ? esc_buf[esc_len - 1] : '\0';
    if (final == 'm') {
        ansi_sgr(esc_buf);
    } else if (final == 'J') {
        vga_clear();
    } else if (final == 'K') {
        erase_current_line();
    } else if (final == 'H') {
        row = 0;
        col = 0;
    }
    esc_state = 0;
    esc_len = 0;
}

void vga_init(void)
{
    color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
    vga_clear();
}

void vga_clear(void)
{
    serial_clear();
    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga_memory[y * VGA_WIDTH + x] = vga_entry(' ', color);
        }
    }
    row = 0;
    col = 0;
    esc_state = 0;
    esc_len = 0;
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
        vga_memory[row * VGA_WIDTH + col] = vga_entry(' ', color);
    } else {
        vga_memory[row * VGA_WIDTH + col] = vga_entry(c, color);
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

/* Move write position + serial cursor WITHOUT touching the hardware cursor.
   Use during bulk screen redraws to avoid cursor flicker on row 0. */
void vga_goto(size_t new_row, size_t new_col)
{
    row = new_row >= VGA_HEIGHT ? VGA_HEIGHT - 1 : new_row;
    col = new_col >= VGA_WIDTH ? VGA_WIDTH - 1 : new_col;
    serial_move_cursor((unsigned)row, (unsigned)col);
}

/* Clear from current column to end of line: \033[K on serial, spaces in VGA memory. */
void vga_erase_eol(void)
{
    serial_erase_line();
    for (size_t x = col; x < VGA_WIDTH; ++x) {
        vga_memory[row * VGA_WIDTH + x] = vga_entry(' ', color);
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
        vga_memory[row * VGA_WIDTH + i] = vga_entry(' ', color);
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
