#ifndef PUREUNIX_PANIC_H
#define PUREUNIX_PANIC_H

void panic(const char *fmt, ...) __attribute__((noreturn));

#endif
