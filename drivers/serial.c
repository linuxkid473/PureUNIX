#include <pureunix/io.h>
#include <pureunix/serial.h>
#include <pureunix/types.h>

#define COM1 0x3F8

static int serial_ready;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    serial_ready = 1;
}

void serial_putc(char c)
{
    if (!serial_ready) {
        return;
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    for (uint32_t i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); ++i) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s) {
        serial_putc(*s++);
    }
}

void serial_write_uint(unsigned value)
{
    char tmp[12];
    size_t len = 0;
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (len) {
        serial_putc(tmp[--len]);
    }
}

void serial_clear(void)
{
    serial_write("\033[2J\033[H");
}

void serial_erase_line(void)
{
    serial_write("\033[K");
}

void serial_move_cursor(unsigned row, unsigned col)
{
    serial_putc('\033');
    serial_putc('[');
    serial_write_uint(row + 1);
    serial_putc(';');
    serial_write_uint(col + 1);
    serial_putc('H');
}
