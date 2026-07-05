/* Console tty driver — the termios-aware half of SYS_READ(fd 0).
 *
 * PureUNIX has exactly one terminal (the VGA console fed by the PS/2
 * keyboard), so unlike a real kernel this isn't per-open-file-description
 * state: there is one struct termios, shared by fds 0/1/2, that persists
 * for the lifetime of the kernel (surviving across processes, same as a
 * real tty survives across the programs run on it).
 *
 * keyboard_getkey() already hands back unbuffered, unechoed keypresses
 * (see drivers/keyboard.c) — everything canonical-mode editing (echo,
 * backspace, line-kill, EOF) and raw-mode passthrough needs is built here
 * on top of that.
 */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/keyboard.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/tty.h>

#define TTY_LINE_MAX 256

static struct termios console_termios;

static void termios_set_defaults(struct termios *t)
{
    memset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL;
    t->c_oflag = OPOST | ONLCR;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    t->c_cc[VINTR]  = 3;    /* ^C */
    t->c_cc[VQUIT]  = 28;   /* ^\ */
    t->c_cc[VERASE] = 127;  /* DEL */
    t->c_cc[VKILL]  = 21;   /* ^U */
    t->c_cc[VEOF]   = 4;    /* ^D */
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;
    t->c_cc[VSUSP]  = 26;   /* ^Z */
}

void tty_init(void)
{
    termios_set_defaults(&console_termios);
}

int tty_get_termios(struct termios *out)
{
    if (!out) {
        return -EINVAL;
    }
    *out = console_termios;
    return 0;
}

int tty_set_termios(const struct termios *in)
{
    if (!in) {
        return -EINVAL;
    }
    console_termios = *in;
    return 0;
}

/* Maps a keyboard_getkey() result to the byte a tty consumer should see.
 * KEY_BACKSPACE (scancode-level, value 8) is normalized to the currently
 * configured VERASE byte so the erase check below has one thing to compare
 * against regardless of which key produced it. Extended keys with no ASCII
 * meaning (arrows, F-keys, ...) return -1 and are dropped — PureUNIX's
 * keyboard driver has no escape-sequence encoding for them yet, so a raw-
 * mode reader (e.g. a line editor) has nothing useful to do with them
 * either. */
static int key_to_byte(int key)
{
    if (key == KEY_ENTER) {
        return '\n';
    }
    if (key == KEY_BACKSPACE) {
        return (int)console_termios.c_cc[VERASE];
    }
    if (key >= 1 && key < 128) {
        return key;
    }
    return -1;
}

static void echo_char(char c)
{
    if (console_termios.c_lflag & ECHO) {
        putchar(c);
    }
}

static void echo_erase(void)
{
    if (console_termios.c_lflag & (ECHO | ECHOE)) {
        putchar('\b');
        putchar(' ');
        putchar('\b');
    }
}

/* Blocks until the next byte the tty layer cares about arrives (silently
 * absorbing keys key_to_byte() has nothing to do with, exactly as SYS_READ's
 * original inline loop did). */
static int next_byte(void)
{
    int b;
    do {
        b = key_to_byte(keyboard_getkey());
    } while (b < 0);
    return b;
}

static int tty_read_canonical(char *buf, size_t len)
{
    const struct termios *t = &console_termios;
    bool sig = (t->c_lflag & ISIG) != 0;
    char line[TTY_LINE_MAX];
    size_t n = 0;

    for (;;) {
        int c = next_byte();

        if (sig && c == t->c_cc[VINTR]) {
            printf("^C\n");
            return -EINTR;
        }
        if (c == t->c_cc[VEOF]) {
            if (n == 0) {
                return 0;
            }
            break;
        }
        if (c == '\n') {
            echo_char('\n');
            line[n < sizeof(line) ? n : sizeof(line) - 1] = '\n';
            n = (n < sizeof(line)) ? n + 1 : sizeof(line);
            break;
        }
        if (c == t->c_cc[VERASE]) {
            if (n > 0) {
                n--;
                echo_erase();
            }
            continue;
        }
        if (c == t->c_cc[VKILL]) {
            while (n > 0) {
                n--;
                echo_erase();
            }
            if (t->c_lflag & ECHOK) {
                echo_char('\n');
            }
            continue;
        }
        if (n < sizeof(line)) {
            line[n++] = (char)c;
            echo_char((char)c);
        }
    }

    size_t to_copy = (n < len) ? n : len;
    memcpy(buf, line, to_copy);
    return (int)to_copy;
}

static int tty_read_raw(char *buf, size_t len)
{
    const struct termios *t = &console_termios;
    bool sig = (t->c_lflag & ISIG) != 0;
    size_t i = 0;

    /* VMIN == 0 with nothing buffered would mean "return immediately, even
     * with zero bytes" (POSIX's polling raw-read mode) — PureUNIX's keyboard
     * queue has no non-blocking peek suitable for that, so every raw read
     * here blocks for the first byte regardless of VMIN/VTIME, then drains
     * whatever else is already queued without blocking again. */
    int first = next_byte();
    if (sig && first == t->c_cc[VINTR]) {
        printf("^C\n");
        return -EINTR;
    }
    buf[i++] = (char)first;
    echo_char((char)first);

    while (i < len) {
        int key = keyboard_try_getkey();
        if (key == KEY_NONE) {
            break;
        }
        int c = key_to_byte(key);
        if (c < 0) {
            continue;
        }
        if (sig && c == t->c_cc[VINTR]) {
            printf("^C\n");
            break;
        }
        buf[i++] = (char)c;
        echo_char((char)c);
    }
    return (int)i;
}

int tty_read(char *buf, size_t len)
{
    if (!buf) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }

    /* SYS_READ reaches here through int $0x80, which (see isr128 in
     * arch/i386/interrupt_stubs.S) runs with interrupts masked from entry
     * until just before its iret. keyboard_getkey()'s wait loop blocks on
     * arch_halt() (hlt) for IRQ1 to deliver the next scancode — with IF
     * still clear from syscall entry, that hlt can never be woken and the
     * whole kernel would hang. Re-enable interrupts before doing any
     * keyboard wait; isr_common_stub's own `sti` before iret makes this
     * unconditionally safe to leave enabled on the way back out. */
    arch_enable_interrupts();

    if (console_termios.c_lflag & ICANON) {
        return tty_read_canonical(buf, len);
    }
    return tty_read_raw(buf, len);
}
