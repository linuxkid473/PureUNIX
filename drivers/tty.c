/* Console tty driver — the termios-aware half of SYS_READ(fd 0).
 *
 * Per-VT: each of kernel/vt.c's NUM_VTS virtual terminals has its own
 * struct termios and its own keyboard input queue (see include/pureunix/
 * vt.h), so this file resolves the *calling task's own* VT (task_t.vt_id)
 * on every call instead of touching one shared, kernel-wide termios the way
 * it used to when PureUNIX had exactly one terminal.
 *
 * vt_input_getkey() already hands back unbuffered, unechoed keypresses,
 * scoped to one VT's own queue (see kernel/vt.c) — everything canonical-mode
 * editing (echo, backspace, line-kill, EOF) and raw-mode passthrough needs
 * is built here on top of that.
 */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/keyboard.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/tty.h>
#include <pureunix/vt.h>

#define TTY_LINE_MAX 256

/* Every fd 0/1/2 read/write ultimately traces back to *some* task; fall
 * back to VT1 (index 0) for the narrow window before kernel_main() calls
 * vt_init() (task_t.vt_id starts at -1 — see kernel/task.c), matching
 * drivers/keyboard.c's own fallback. */
static int caller_vt_id(void)
{
    task_t *t = task_current();
    int vt_id = t ? t->vt_id : -1;
    return vt_id >= 0 ? vt_id : 0;
}

void tty_init(void)
{
    /* Per-VT termios is seeded by kernel/vt.c's vt_init() (called right
     * after this from kernel_main()) — nothing to do here. Kept as a no-op
     * so the existing boot-sequence call site doesn't need to change. */
}

int tty_get_termios(struct termios *out)
{
    return vt_get_termios(caller_vt_id(), out);
}

int tty_set_termios(const struct termios *in)
{
    return vt_set_termios(caller_vt_id(), in);
}

/* Maps a vt_input_getkey() result to the byte a tty consumer should see.
 * KEY_BACKSPACE (scancode-level, value 8) is normalized to the currently
 * configured VERASE byte so the erase check below has one thing to compare
 * against regardless of which key produced it. KEY_CTRL_C/Z/BACKSLASH
 * (drivers/keyboard.c/hid.c — sentinel values above 128, not raw bytes)
 * are normalized to their real ASCII control-code bytes (0x03/0x1A/0x1C)
 * so the VINTR/VSUSP/VQUIT comparisons below (and a caller's own
 * tcsetattr()-configured control characters, if changed from the default)
 * see the same byte a real PS/2 controller sending those raw control
 * codes directly would have produced. Extended keys with no ASCII meaning
 * (arrows, F-keys, ...) return -1 and are dropped. */
static int key_to_byte(const struct termios *t, int key)
{
    if (key == KEY_ENTER) {
        /* '\r' (0x0D/CR), not '\n' (0x0A/LF) — the real, conventional byte
         * every physical terminal's Return key transmits in raw/cbreak
         * mode (canonical-mode line assembly below still accepts either,
         * so ordinary shell input is unaffected). Found because ncurses
         * apps that call nonl() (htop's CRT_init(), third_party/htop/ —
         * disabling ncurses' own default automatic \r->\n input
         * translation, precisely so they can see this raw byte
         * themselves) check the raw code for 13 explicitly (e.g. Action.c's
         * list-picker "confirm selection" handler) and never matched
         * against the '\n' this used to send — Enter silently did nothing
         * in htop's sort-by/kill-signal panels. Apps that *don't* call
         * nonl() (e.g. user/ncdemo.c) are unaffected: ncurses' own default
         * nl() mode already translates a raw '\r' back to '\n' before
         * wgetch() returns it, exactly like a real terminal/ncurses pair
         * anywhere else. */
        return '\r';
    }
    if (key == KEY_BACKSPACE) {
        return (int)t->c_cc[VERASE];
    }
    if (key == KEY_CTRL_C) {
        return 0x03;
    }
    if (key == KEY_CTRL_Z) {
        return 0x1A;
    }
    if (key == KEY_CTRL_BACKSLASH) {
        return 0x1C;
    }
    if (key >= 1 && key < 128) {
        return key;
    }
    return -1;
}

/* Extended keys with no direct ASCII meaning (arrows, Home/End/PgUp/PgDn/
 * Delete, F1..F12) — key_to_byte() above returns -1 for all of these.
 * Translated to real ANSI/xterm terminal-input escape sequences here,
 * matching this port's "pureunix" terminfo entry (docs/ncurses-port.md)
 * exactly, so ncurses' getch()/KEY_* resolution sees the same bytes a real
 * terminal emitting that terminfo's kcuu1/kcud1/.../kf1..kf12 would send.
 * Returns the sequence length (<= PENDING_MAX), or 0 if `key` has no
 * escape-sequence mapping either (a genuinely unhandled key). */
#define PENDING_MAX 8

static size_t key_to_escape_seq(int key, char *out)
{
    switch (key) {
    case KEY_UP:        memcpy(out, "\033[A", 3); return 3;
    case KEY_DOWN:      memcpy(out, "\033[B", 3); return 3;
    case KEY_RIGHT:     memcpy(out, "\033[C", 3); return 3;
    case KEY_LEFT:      memcpy(out, "\033[D", 3); return 3;
    case KEY_HOME:      memcpy(out, "\033[H", 3); return 3;
    case KEY_END:       memcpy(out, "\033[F", 3); return 3;
    case KEY_PAGE_UP:   memcpy(out, "\033[5~", 4); return 4;
    case KEY_PAGE_DOWN: memcpy(out, "\033[6~", 4); return 4;
    case KEY_DELETE:    memcpy(out, "\033[3~", 4); return 4;
    case KEY_F1:        memcpy(out, "\033OP", 3); return 3;
    case KEY_F2:        memcpy(out, "\033OQ", 3); return 3;
    case KEY_F3:        memcpy(out, "\033OR", 3); return 3;
    case KEY_F4:        memcpy(out, "\033OS", 3); return 3;
    case KEY_F5:        memcpy(out, "\033[15~", 5); return 5;
    case KEY_F6:        memcpy(out, "\033[17~", 5); return 5;
    case KEY_F7:        memcpy(out, "\033[18~", 5); return 5;
    case KEY_F8:        memcpy(out, "\033[19~", 5); return 5;
    case KEY_F9:        memcpy(out, "\033[20~", 5); return 5;
    case KEY_F10:       memcpy(out, "\033[21~", 5); return 5;
    case KEY_F11:       memcpy(out, "\033[23~", 5); return 5;
    case KEY_F12:       memcpy(out, "\033[24~", 5); return 5;
    default:             return 0;
    }
}

/* One escape-sequence queue per VT (indexed like every other per-VT input
 * state in kernel/vt.c) — a special key produces several bytes from one
 * keypress, but tty_read() hands them back to its caller one at a time (or
 * however many fit in the caller's buffer), so the not-yet-returned tail
 * has to survive across separate tty_read()/SYS_READ calls, not just
 * within one. */
static char pending_buf[NUM_VTS][PENDING_MAX];
static uint8_t pending_len[NUM_VTS];
static uint8_t pending_pos[NUM_VTS];

/* Non-blocking: drains a queued escape-sequence byte if one is pending,
 * else tries one already-buffered key (vt_input_try_getkey() never blocks),
 * queuing the rest of its escape sequence if it turns out to need one.
 * Returns -1 if nothing is available right now. */
static int try_next_byte(int vt_id, const struct termios *t)
{
    if (pending_pos[vt_id] < pending_len[vt_id]) {
        return (unsigned char)pending_buf[vt_id][pending_pos[vt_id]++];
    }
    for (;;) {
        int key = vt_input_try_getkey(vt_id);
        if (key == 0 /* KEY_NONE */) {
            return -1;
        }
        int b = key_to_byte(t, key);
        if (b >= 0) {
            return b;
        }
        size_t seqlen = key_to_escape_seq(key, pending_buf[vt_id]);
        if (seqlen > 0) {
            pending_len[vt_id] = (uint8_t)seqlen;
            pending_pos[vt_id] = 1;
            return (unsigned char)pending_buf[vt_id][0];
        }
        /* Stray key with no byte or escape-sequence mapping: keep draining
         * without blocking, matching this loop's pre-existing behavior. */
    }
}

static void echo_char(int vt_id, const struct termios *t, char c)
{
    if (t->c_lflag & ECHO) {
        vt_putc(vt_id, c);
    }
}

static void echo_erase(int vt_id, const struct termios *t)
{
    if (t->c_lflag & (ECHO | ECHOE)) {
        vt_putc(vt_id, '\b');
        vt_putc(vt_id, ' ');
        vt_putc(vt_id, '\b');
    }
}

/* Blocks until the next byte the tty layer cares about arrives (silently
 * absorbing keys key_to_byte() has nothing to do with). Blocks on VT
 * vt_id's own input queue — fed only while vt_id is the active VT (see
 * vt_input_push()), so a backgrounded VT's read blocks here until it's
 * switched back to, exactly like a real Linux VT. */
static int next_byte(int vt_id, const struct termios *t)
{
    for (;;) {
        if (pending_pos[vt_id] < pending_len[vt_id]) {
            return (unsigned char)pending_buf[vt_id][pending_pos[vt_id]++];
        }
        int key = vt_input_getkey(vt_id);
        int b = key_to_byte(t, key);
        if (b >= 0) {
            return b;
        }
        size_t seqlen = key_to_escape_seq(key, pending_buf[vt_id]);
        if (seqlen > 0) {
            pending_len[vt_id] = (uint8_t)seqlen;
            pending_pos[vt_id] = 0;
        }
        /* Neither an ASCII-mapped key nor one with an escape-sequence
         * mapping (a genuinely unhandled key, or the KEY_NONE the block
         * above never actually queues towards) — block for the next one. */
    }
}

/* Ctrl+C/Ctrl+Z/Ctrl+\ are handled entirely below this layer now —
 * kernel/vt.c's vt_input_push() intercepts them the instant they arrive
 * from the keyboard IRQ (when ISIG is set) and delivers a real signal
 * straight to the VT's foreground process group, without ever queuing
 * them as ordinary input data. This is deliberately *not* done here in
 * the read() path: a foreground job that never calls read() at all (a
 * CPU-bound loop, `sleep`, ...) still has to be interruptible by
 * Ctrl+C, exactly like a real Unix line discipline — coupling signal
 * generation to a specific read() call would silently fail to interrupt
 * exactly those cases. So by the time next_byte() below ever returns a
 * value, it is never one of those three control characters (with ISIG
 * on); see docs/process-management.md. */
static int tty_read_canonical(int vt_id, const struct termios *t, char *buf, size_t len)
{
    char line[TTY_LINE_MAX];
    size_t n = 0;

    for (;;) {
        int c = next_byte(vt_id, t);

        if (c == t->c_cc[VEOF]) {
            if (n == 0) {
                return 0;
            }
            break;
        }
        if (c == '\n' || c == '\r') {
            /* Accept either — KEY_ENTER now produces the real, raw '\r'
             * terminal convention (key_to_byte() above), but the line
             * itself always ends with a literal '\n', matching what
             * every canonical-mode reader (fgets(), BusyBox's own
             * non-raw-mode fallback, ...) already expects. */
            echo_char(vt_id, t, '\n');
            line[n < sizeof(line) ? n : sizeof(line) - 1] = '\n';
            n = (n < sizeof(line)) ? n + 1 : sizeof(line);
            break;
        }
        if (c == t->c_cc[VERASE]) {
            if (n > 0) {
                n--;
                echo_erase(vt_id, t);
            }
            continue;
        }
        if (c == t->c_cc[VKILL]) {
            while (n > 0) {
                n--;
                echo_erase(vt_id, t);
            }
            if (t->c_lflag & ECHOK) {
                echo_char(vt_id, t, '\n');
            }
            continue;
        }
        if (n < sizeof(line)) {
            line[n++] = (char)c;
            echo_char(vt_id, t, (char)c);
        }
    }

    size_t to_copy = (n < len) ? n : len;
    memcpy(buf, line, to_copy);
    return (int)to_copy;
}

static int tty_read_raw(int vt_id, const struct termios *t, char *buf, size_t len)
{
    size_t i = 0;

    /* VMIN == 0 with nothing buffered would mean "return immediately, even
     * with zero bytes" (POSIX's polling raw-read mode) — PureUNIX's keyboard
     * queue has no non-blocking peek suitable for that, so every raw read
     * here blocks for the first byte regardless of VMIN/VTIME, then drains
     * whatever else is already queued without blocking again. Ctrl+C/
     * Ctrl+Z/Ctrl+\ are handled below this layer (kernel/vt.c's
     * vt_input_push()) — see tty_read_canonical()'s header comment. */
    int first = next_byte(vt_id, t);
    buf[i++] = (char)first;
    echo_char(vt_id, t, (char)first);

    while (i < len) {
        int c = try_next_byte(vt_id, t);
        if (c < 0) {
            break;
        }
        buf[i++] = (char)c;
        echo_char(vt_id, t, (char)c);
    }
    return (int)i;
}

int tty_read(int vt_id, char *buf, size_t len)
{
    if (!buf) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }

    /* SYS_READ reaches here through int $0x80, which (see isr128 in
     * arch/i386/interrupt_stubs.S) runs with interrupts masked from entry
     * until just before its iret. vt_input_getkey()'s wait loop blocks on
     * arch_halt() (hlt) for IRQ1 (or a USB keyboard's interrupt transfer
     * completion) to deliver the next scancode — with IF still clear from
     * syscall entry, that hlt can never be woken and the whole kernel would
     * hang. Re-enable interrupts before doing any keyboard wait; isr_common_
     * stub's own `sti` before iret makes this unconditionally safe to leave
     * enabled on the way back out. */
    arch_enable_interrupts();

    struct termios t;
    vt_get_termios(vt_id, &t);

    if (t.c_lflag & ICANON) {
        return tty_read_canonical(vt_id, &t, buf, len);
    }
    return tty_read_raw(vt_id, &t, buf, len);
}
