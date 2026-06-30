#ifndef LIBPURE_H
#define LIBPURE_H

#include <stddef.h>
#include <stdint.h>

int pu_write(int fd, const char *buf, size_t len);
int pu_read(int fd, char *buf, size_t len);
void pu_puts(const char *s);
void pu_puti(int value);
size_t pu_strlen(const char *s);
int pu_atoi(const char *s);

#endif
