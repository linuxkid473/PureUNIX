#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vga.h>

typedef struct outbuf {
    char *buf;
    size_t size;
    size_t pos;
    size_t total;
    bool terminal;
} outbuf_t;

static void emit(outbuf_t *out, char c)
{
    if (out->terminal) {
        vga_putc(c);
    } else if (out->buf && out->size > 0 && out->pos + 1 < out->size) {
        out->buf[out->pos++] = c;
    }
    out->total++;
}

static void emit_str(outbuf_t *out, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        emit(out, *s++);
    }
}

static void emit_number(outbuf_t *out, uint32_t value, uint32_t base, bool sign, bool upper)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (sign && (int32_t)value < 0) {
        emit(out, '-');
        value = (uint32_t)(-(int32_t)value);
    }
    if (value == 0) {
        emit(out, '0');
        return;
    }
    while (value) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i--) {
        emit(out, tmp[i]);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    outbuf_t out = { .buf = buf, .size = size, .pos = 0, .total = 0, .terminal = false };

    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            emit(&out, *p);
            continue;
        }
        ++p;
        bool long_arg = false;
        if (*p == 'l') {
            long_arg = true;
            ++p;
        }
        switch (*p) {
        case '%':
            emit(&out, '%');
            break;
        case 'c':
            emit(&out, (char)va_arg(args, int));
            break;
        case 's':
            emit_str(&out, va_arg(args, const char *));
            break;
        case 'd':
        case 'i':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, true, false);
            break;
        case 'u':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, false, false);
            break;
        case 'x':
            emit_number(&out, va_arg(args, uint32_t), 16, false, false);
            break;
        case 'X':
            emit_number(&out, va_arg(args, uint32_t), 16, false, true);
            break;
        case 'p':
            emit_str(&out, "0x");
            emit_number(&out, (uint32_t)va_arg(args, void *), 16, false, false);
            break;
        default:
            emit(&out, '%');
            emit(&out, *p);
            break;
        }
    }

    if (buf && size > 0) {
        buf[out.pos < size ? out.pos : size - 1] = '\0';
    }
    return (int)out.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}

int vprintf(const char *fmt, va_list args)
{
    outbuf_t out = { .terminal = true };
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            emit(&out, *p);
            continue;
        }
        ++p;
        bool long_arg = false;
        if (*p == 'l') {
            long_arg = true;
            ++p;
        }
        switch (*p) {
        case '%':
            emit(&out, '%');
            break;
        case 'c':
            emit(&out, (char)va_arg(args, int));
            break;
        case 's':
            emit_str(&out, va_arg(args, const char *));
            break;
        case 'd':
        case 'i':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, true, false);
            break;
        case 'u':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, false, false);
            break;
        case 'x':
            emit_number(&out, va_arg(args, uint32_t), 16, false, false);
            break;
        case 'X':
            emit_number(&out, va_arg(args, uint32_t), 16, false, true);
            break;
        case 'p':
            emit_str(&out, "0x");
            emit_number(&out, (uint32_t)va_arg(args, void *), 16, false, false);
            break;
        default:
            emit(&out, '%');
            emit(&out, *p);
            break;
        }
    }
    return (int)out.total;
}

int printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vprintf(fmt, args);
    va_end(args);
    return r;
}

void putchar(int ch)
{
    vga_putc((char)ch);
}

void puts(const char *str)
{
    vga_write(str);
    vga_putc('\n');
}
