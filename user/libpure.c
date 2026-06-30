#include "libpure.h"

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

int pu_write(int fd, const char *buf, size_t len)
{
    return syscall3(2, fd, (int)buf, (int)len);
}

int pu_read(int fd, char *buf, size_t len)
{
    return syscall3(3, fd, (int)buf, (int)len);
}

size_t pu_strlen(const char *s)
{
    size_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

void pu_puts(const char *s)
{
    pu_write(1, s, pu_strlen(s));
}

void pu_puti(int value)
{
    char buf[16];
    int i = 0;
    if (value == 0) {
        pu_puts("0");
        return;
    }
    if (value < 0) {
        pu_puts("-");
        value = -value;
    }
    while (value) {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (i--) {
        pu_write(1, &buf[i], 1);
    }
}

int pu_atoi(const char *s)
{
    int sign = 1;
    int value = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s++ - '0');
    }
    return value * sign;
}
