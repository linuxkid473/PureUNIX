#include <pureunix/memory.h>
#include <pureunix/string.h>

void *memset(void *dest, int value, size_t count)
{
    uint8_t *d = dest;
    while (count--) {
        *d++ = (uint8_t)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (d < s) {
        while (count--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += count;
        s += count;
        while (count--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (size_t i = 0; i < count; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *str)
{
    size_t len = 0;
    while (str && str[len]) {
        ++len;
    }
    return len;
}

size_t strnlen(const char *str, size_t max)
{
    size_t len = 0;
    while (str && len < max && str[len]) {
        ++len;
    }
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *out = dest;
    while ((*dest++ = *src++) != '\0') {
    }
    return out;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; ++i) {
        dest[i] = src[i];
    }
    for (; i < n; ++i) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i] || !b[i]) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

char *strchr(const char *str, int ch)
{
    while (*str) {
        if (*str == (char)ch) {
            return (char *)str;
        }
        ++str;
    }
    return ch == 0 ? (char *)str : NULL;
}

char *strrchr(const char *str, int ch)
{
    const char *last = NULL;
    do {
        if (*str == (char)ch) {
            last = str;
        }
    } while (*str++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return (char *)haystack;
    }
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        if (*p == *needle && strncmp(p, needle, nlen) == 0) {
            return (char *)p;
        }
    }
    return NULL;
}

char *strdup(const char *src)
{
    size_t len = strlen(src) + 1;
    char *copy = kmalloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, len);
    return copy;
}

static bool delim_has(const char *delim, char c)
{
    while (*delim) {
        if (*delim++ == c) {
            return true;
        }
    }
    return false;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *s = str ? str : *saveptr;
    if (!s) {
        return NULL;
    }
    while (*s && delim_has(delim, *s)) {
        ++s;
    }
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }
    char *token = s;
    while (*s && !delim_has(delim, *s)) {
        ++s;
    }
    if (*s) {
        *s++ = '\0';
    }
    *saveptr = s;
    return token;
}
