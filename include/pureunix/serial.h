#ifndef PUREUNIX_SERIAL_H
#define PUREUNIX_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_clear(void);
void serial_erase_line(void);
void serial_move_cursor(unsigned row, unsigned col);

#endif
