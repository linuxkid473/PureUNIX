#ifndef PUREUNIX_STDIO_H
#define PUREUNIX_STDIO_H

#include <stdarg.h>
#include <pureunix/types.h>

void putchar(int ch);
void puts(const char *str);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif
