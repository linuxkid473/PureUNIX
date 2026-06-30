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

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *str);
void vga_write_len(const char *str, size_t len);
void vga_set_color(uint8_t fg, uint8_t bg);
uint8_t vga_color(void);
void vga_get_cursor(size_t *row, size_t *col);
void vga_goto(size_t row, size_t col);
void vga_erase_eol(void);
void vga_set_cursor(size_t row, size_t col);
void vga_move_cursor_rel(int drow, int dcol);
void vga_status_bar(const char *left, const char *right);

#endif
