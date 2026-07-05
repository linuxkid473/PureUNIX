/* PureUNIX platform shim for the vendored Neatvi sources: implements the
 * bits of libc (malloc family, file I/O, string/ctype, printf) that the
 * headers under user/vi/compat/ declare, backed by user/libpure.h's pu_*
 * syscalls. Nothing here is part of upstream Neatvi. */
#include <stdarg.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include "libpure.h"

/* Forward declarations for functions used above their own definition
 * further down in this file. */
int isspace(int c);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
long strtol(const char *s, char **endptr, int base);

/* ------------------------------------------------------------------ */
/* malloc/realloc/free: first-fit free list over a static arena. The user
 * window (USER_WINDOW_BASE..USER_WINDOW_END-USER_STACK_SIZE, see
 * include/pureunix/vmm.h) is ~2.97 MiB for code+data+bss combined, so 1 MiB
 * of that reserved for the heap leaves ample room for Neatvi's own code. */
/* ------------------------------------------------------------------ */

#define ARENA_SIZE (1024U * 1024U)

typedef struct block_hdr {
    unsigned int size;   /* usable bytes following this header */
    int free;
    struct block_hdr *next;
    struct block_hdr *prev;
} block_hdr_t;

static unsigned char arena[ARENA_SIZE];
static block_hdr_t *heap_head;

static size_t align8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

static void heap_ensure_init(void)
{
    if (heap_head) {
        return;
    }
    heap_head = (block_hdr_t *)arena;
    heap_head->size = ARENA_SIZE - sizeof(block_hdr_t);
    heap_head->free = 1;
    heap_head->next = NULL;
    heap_head->prev = NULL;
}

static void split_block(block_hdr_t *b, size_t size)
{
    if (b->size < size + sizeof(block_hdr_t) + 16) {
        return;
    }
    block_hdr_t *rest = (block_hdr_t *)((unsigned char *)(b + 1) + size);
    rest->size = b->size - (unsigned int)size - (unsigned int)sizeof(block_hdr_t);
    rest->free = 1;
    rest->next = b->next;
    rest->prev = b;
    if (rest->next) {
        rest->next->prev = rest;
    }
    b->next = rest;
    b->size = (unsigned int)size;
}

static void coalesce(block_hdr_t *b)
{
    if (b->next && b->next->free) {
        b->size += (unsigned int)sizeof(block_hdr_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) {
            b->next->prev = b;
        }
    }
    if (b->prev && b->prev->free) {
        coalesce(b->prev);
    }
}

void *malloc(size_t size)
{
    heap_ensure_init();
    if (!size) {
        return NULL;
    }
    size = align8(size);
    for (block_hdr_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            return b + 1;
        }
    }
    return NULL;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void free(void *ptr)
{
    if (!ptr) {
        return;
    }
    block_hdr_t *b = (block_hdr_t *)ptr - 1;
    b->free = 1;
    coalesce(b);
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return malloc(size);
    }
    if (!size) {
        free(ptr);
        return NULL;
    }
    block_hdr_t *b = (block_hdr_t *)ptr - 1;
    if (b->size >= size) {
        return ptr;
    }
    void *n = malloc(size);
    if (!n) {
        return NULL;
    }
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

/* ------------------------------------------------------------------ */
/* file I/O — thin wrappers over libpure's pu_* syscalls                */
/* ------------------------------------------------------------------ */

int open(const char *path, int flags, ...)
{
    return pu_open(path, flags);
}

long read(int fd, void *buf, size_t count)
{
    return pu_read(fd, buf, count);
}

long write(int fd, const void *buf, size_t count)
{
    return pu_write(fd, buf, count);
}

int close(int fd)
{
    return pu_close(fd);
}

long lseek(int fd, long offset, int whence)
{
    return pu_lseek(fd, (int)offset, whence);
}

int unlink(const char *path)
{
    return pu_unlink(path);
}

int access(const char *path, int mode)
{
    return pu_access(path, mode);
}

int ftruncate(int fd, long length)
{
    (void)fd;
    (void)length;
    return 0;
}

/* ex.c only ever reads st_mtime back out of this (to notice a file changed
 * on disk under it); st here is really a `struct stat *` per compat's
 * sys/stat.h (a single `long st_mtime` field) — deliberately typed as
 * void* so this file never has to see two conflicting `struct stat`
 * definitions (this one's and libpure.h's full one) at once. Writing
 * exactly one `long` keeps it within the caller's actual allocation. */
int stat(const char *path, void *st)
{
    struct stat full;
    if (pu_stat(path, &full) != 0) {
        return -1;
    }
    *(long *)st = (long)full.st_mtime;
    return 0;
}

/* ------------------------------------------------------------------ */
/* misc libc                                                            */
/* ------------------------------------------------------------------ */

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

void exit(int code)
{
    pu_exit(code);
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long strtol(const char *s, char **endptr, int base)
{
    long sign = 1;
    long value = 0;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'z') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'Z') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        s++;
    }
    if (endptr) {
        *endptr = (char *)s;
    }
    return value * sign;
}

sighandler_t signal(int signum, sighandler_t handler)
{
    (void)signum;
    (void)handler;
    return SIG_DFL;
}

int kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}

int raise(int sig)
{
    (void)sig;
    return 0;
}

/* ------------------------------------------------------------------ */
/* string.h / ctype.h                                                   */
/* ------------------------------------------------------------------ */

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++)) {
    }
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n) {
        return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = c == '\0' ? s + strlen(s) : NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen) {
        return (char *)hay;
    }
    for (; *hay; hay++) {
        if (strncmp(hay, needle, nlen) == 0) {
            return (char *)hay;
        }
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isprint(int c)
{
    return c >= 0x20 && c < 0x7f;
}

int tolower(int c)
{
    return isupper(c) ? c - 'A' + 'a' : c;
}

int toupper(int c)
{
    return islower(c) ? c - 'a' + 'A' : c;
}

/* ------------------------------------------------------------------ */
/* printf family — upstream Neatvi uses width + zero-padding (%04d,
 * %04x, %2i) that PureUNIX's own kernel vsnprintf doesn't support, so
 * this is a fuller implementation local to this program. */
/* ------------------------------------------------------------------ */

static void emit(char *buf, size_t size, size_t *pos, char c)
{
    if (buf && *pos + 1 < size) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void emit_str(char *buf, size_t size, size_t *pos, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        emit(buf, size, pos, *s++);
    }
}

static void emit_number(char *buf, size_t size, size_t *pos, long value, int base,
                         int is_signed, int upper, int width, int zero_pad)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    int neg = 0;
    unsigned long uv;

    if (is_signed && value < 0) {
        neg = 1;
        uv = (unsigned long)(-value);
    } else {
        uv = (unsigned long)value;
    }
    if (uv == 0) {
        tmp[n++] = '0';
    }
    while (uv) {
        tmp[n++] = digits[uv % (unsigned)base];
        uv /= (unsigned)base;
    }
    int total = n + (neg ? 1 : 0);
    if (neg && zero_pad) {
        emit(buf, size, pos, '-');
    }
    for (int i = total; i < width; i++) {
        emit(buf, size, pos, zero_pad ? '0' : ' ');
    }
    if (neg && !zero_pad) {
        emit(buf, size, pos, '-');
    }
    while (n--) {
        emit(buf, size, pos, tmp[n]);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(buf, size, &pos, *p);
            continue;
        }
        p++;
        int zero_pad = 0;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        int long_arg = 0;
        if (*p == 'l') {
            long_arg = 1;
            p++;
        }
        switch (*p) {
        case '%':
            emit(buf, size, &pos, '%');
            break;
        case 'c':
            emit(buf, size, &pos, (char)va_arg(args, int));
            break;
        case 's':
            emit_str(buf, size, &pos, va_arg(args, const char *));
            break;
        case 'd':
        case 'i':
            emit_number(buf, size, &pos, long_arg ? va_arg(args, long) : va_arg(args, int),
                        10, 1, 0, width, zero_pad);
            break;
        case 'u':
            emit_number(buf, size, &pos,
                        (long)(long_arg ? va_arg(args, unsigned long) : va_arg(args, unsigned int)),
                        10, 0, 0, width, zero_pad);
            break;
        case 'x':
            emit_number(buf, size, &pos,
                        (long)(long_arg ? va_arg(args, unsigned long) : va_arg(args, unsigned int)),
                        16, 0, 0, width, zero_pad);
            break;
        case 'X':
            emit_number(buf, size, &pos,
                        (long)(long_arg ? va_arg(args, unsigned long) : va_arg(args, unsigned int)),
                        16, 0, 1, width, zero_pad);
            break;
        case 'p':
            emit_str(buf, size, &pos, "0x");
            emit_number(buf, size, &pos, (long)(unsigned long)va_arg(args, void *), 16, 0, 0, 0, 0);
            break;
        default:
            emit(buf, size, &pos, '%');
            if (*p) {
                emit(buf, size, &pos, *p);
            }
            break;
        }
    }
    if (buf && size > 0) {
        buf[pos < size ? pos : size - 1] = '\0';
    }
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, args);
    va_end(args);
    return r;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    write(1, tmp, (size_t)r < sizeof(tmp) ? (size_t)r : sizeof(tmp) - 1);
    return r;
}

int printf(const char *fmt, ...)
{
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    write(1, tmp, (size_t)r < sizeof(tmp) ? (size_t)r : sizeof(tmp) - 1);
    return r;
}

int getchar(void)
{
    unsigned char c;
    long n = read(0, &c, 1);
    return n == 1 ? (int)c : EOF;
}
