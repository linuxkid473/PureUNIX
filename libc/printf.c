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

static void emit_number(outbuf_t *out, uint32_t value, uint32_t base, bool sign, bool upper,
                         int width, bool zero_pad)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    bool negative = sign && (int32_t)value < 0;
    if (negative) {
        value = (uint32_t)(-(int32_t)value);
    }
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }
    if (negative) {
        emit(out, '-');
    }
    int pad = width - i - (negative ? 1 : 0);
    for (int j = 0; j < pad; ++j) {
        emit(out, zero_pad ? '0' : ' ');
    }
    while (i--) {
        emit(out, tmp[i]);
    }
}

/* Parses an optional "0" zero-pad flag followed by optional decimal width
 * digits (e.g. the "02" in "%02x") -- the only two printf() features this
 * freestanding implementation supports beyond a bare conversion letter. */
static void parse_width(const char **pp, int *width, bool *zero_pad)
{
    const char *p = *pp;
    *zero_pad = false;
    *width = 0;
    if (*p == '0') {
        *zero_pad = true;
        ++p;
    }
    while (*p >= '0' && *p <= '9') {
        *width = (*width) * 10 + (*p - '0');
        ++p;
    }
    *pp = p;
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
        int width;
        bool zero_pad;
        parse_width(&p, &width, &zero_pad);
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
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, true, false, width, zero_pad);
            break;
        case 'u':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, false, false, width, zero_pad);
            break;
        case 'x':
            emit_number(&out, va_arg(args, uint32_t), 16, false, false, width, zero_pad);
            break;
        case 'X':
            emit_number(&out, va_arg(args, uint32_t), 16, false, true, width, zero_pad);
            break;
        case 'p':
            emit_str(&out, "0x");
            emit_number(&out, (uint32_t)va_arg(args, void *), 16, false, false, 0, false);
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
        int width;
        bool zero_pad;
        parse_width(&p, &width, &zero_pad);
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
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, true, false, width, zero_pad);
            break;
        case 'u':
            emit_number(&out, long_arg ? va_arg(args, uint32_t) : va_arg(args, uint32_t), 10, false, false, width, zero_pad);
            break;
        case 'x':
            emit_number(&out, va_arg(args, uint32_t), 16, false, false, width, zero_pad);
            break;
        case 'X':
            emit_number(&out, va_arg(args, uint32_t), 16, false, true, width, zero_pad);
            break;
        case 'p':
            emit_str(&out, "0x");
            emit_number(&out, (uint32_t)va_arg(args, void *), 16, false, false, 0, false);
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
