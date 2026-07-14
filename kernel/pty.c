/* kernel/pty.c — real PTY (pseudo-terminal) pairs. See include/pureunix/
 * pty.h for the design rationale; this mirrors kernel/vt.c (termios,
 * fg_pgid job control, ISIG interception at the point input arrives) and
 * drivers/tty.c (canonical-mode line discipline) over a pair of ring
 * buffers instead of a VGA console + keyboard queue. */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/pty.h>
#include <pureunix/signal.h>
#include <pureunix/string.h>
#include <pureunix/task.h>

#define PTY_BUF_SIZE 4096
/* A handful of concurrent ptys is all any real use of this primitive needs
 * (PUTerm creates exactly one) — a fixed pool, same style as
 * MAX_OPEN_FILE_DESCRIPTIONS (kernel/task.c). */
#define MAX_PTYS 8

typedef struct {
    uint8_t data[PTY_BUF_SIZE];
    size_t head, tail, count;
} pty_ring_t;

struct pty {
    bool in_use;

    /* master -> slave: what the user typed (PUTerm's writes). Read by the
     * slave (the shell) with real canonical-mode line discipline. */
    pty_ring_t in;
    wait_queue_t in_read_wq;  /* woken when `in` gains data (slave blocked reading) */
    wait_queue_t in_write_wq; /* woken when `in` gains space (master blocked writing) */

    /* slave -> master: the shell's stdout/stderr. Read (non-blocking) by
     * the master (PUTerm's render loop). */
    pty_ring_t out;
    wait_queue_t out_write_wq; /* woken when `out` gains space (slave blocked writing) */

    int master_ends;
    int slave_ends;

    struct termios termios;
    int fg_pgid;
    int sid;
    unsigned short rows, cols;
};

static pty_t g_ptys[MAX_PTYS];

static void termios_defaults(struct termios *t)
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

pty_t *pty_alloc(void)
{
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!g_ptys[i].in_use) {
            memset(&g_ptys[i], 0, sizeof(g_ptys[i]));
            g_ptys[i].in_use = true;
            termios_defaults(&g_ptys[i].termios);
            g_ptys[i].rows = 24;
            g_ptys[i].cols = 80;
            return &g_ptys[i];
        }
    }
    return NULL;
}

void pty_master_ref(pty_t *p)
{
    if (p) {
        p->master_ends++;
    }
}

void pty_slave_ref(pty_t *p)
{
    if (p) {
        p->slave_ends++;
    }
}

void pty_master_unref(pty_t *p)
{
    if (!p) {
        return;
    }
    if (--p->master_ends <= 0) {
        p->master_ends = 0;
        /* No reader left for the shell's stdout/stderr — wake any writer
         * blocked on a full `out` ring so it sees -EPIPE instead of
         * hanging forever, matching pipe_write()'s own EPIPE-on-no-readers
         * behavior (kernel/task.c's pipe handling). */
        wait_queue_wake_all(&p->out_write_wq);
    }
    if (p->master_ends == 0 && p->slave_ends == 0) {
        p->in_use = false;
    }
}

void pty_slave_unref(pty_t *p)
{
    if (!p) {
        return;
    }
    if (--p->slave_ends <= 0) {
        p->slave_ends = 0;
        /* No shell left to read `in` or write `out` — wake anyone blocked
         * on either so they see EOF/EPIPE instead of hanging. */
        wait_queue_wake_all(&p->in_write_wq);
    }
    if (p->master_ends == 0 && p->slave_ends == 0) {
        p->in_use = false;
    }
}

static size_t ring_push(pty_ring_t *r, const char *buf, size_t len)
{
    size_t n = 0;
    while (n < len && r->count < PTY_BUF_SIZE) {
        r->data[r->head] = (uint8_t)buf[n++];
        r->head = (r->head + 1) % PTY_BUF_SIZE;
        r->count++;
    }
    return n;
}

static size_t ring_pop(pty_ring_t *r, char *buf, size_t len)
{
    size_t n = 0;
    while (n < len && r->count > 0) {
        buf[n++] = (char)r->data[r->tail];
        r->tail = (r->tail + 1) % PTY_BUF_SIZE;
        r->count--;
    }
    return n;
}

/* ---- Wait predicates (shared by both directions) ----------------------- */

typedef struct {
    pty_t *p;
} pty_wait_ctx_t;

static bool in_readable(void *ctx_)
{
    pty_wait_ctx_t *ctx = ctx_;
    return ctx->p->in.count > 0 || ctx->p->master_ends == 0;
}

static bool in_writable(void *ctx_)
{
    pty_wait_ctx_t *ctx = ctx_;
    return ctx->p->in.count < PTY_BUF_SIZE || ctx->p->slave_ends == 0;
}

static bool out_writable(void *ctx_)
{
    pty_wait_ctx_t *ctx = ctx_;
    return ctx->p->out.count < PTY_BUF_SIZE || ctx->p->master_ends == 0;
}

/* ---- Master side -------------------------------------------------------- */

int pty_master_read(pty_t *p, char *buf, size_t len)
{
    if (!p || !buf) {
        return -EINVAL;
    }
    /* Deliberately never blocks -- see include/pureunix/pty.h. */
    return (int)ring_pop(&p->out, buf, len);
}

/* Same KEY-to-signal-byte spirit as kernel/vt.c's key_to_signal_byte(), but
 * simpler: the master already writes raw bytes (no KEY_* translation layer
 * exists on this side), so there's nothing to normalize -- just compare
 * directly against the pty's own configured control characters. */
static int signal_for_byte(const struct termios *t, int b)
{
    if (b == (int)t->c_cc[VINTR]) {
        return SIGINT;
    }
    if (b == (int)t->c_cc[VQUIT]) {
        return SIGQUIT;
    }
    if (b == (int)t->c_cc[VSUSP]) {
        return SIGTSTP;
    }
    return 0;
}

static const char *echo_for_signal(int sig)
{
    if (sig == SIGINT) return "^C\n";
    if (sig == SIGQUIT) return "^\\\n";
    if (sig == SIGTSTP) return "^Z\n";
    return "";
}

int pty_master_write(pty_t *p, const char *buf, size_t len)
{
    if (!p || !buf) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    size_t written = 0;
    while (written < len) {
        if (p->slave_ends == 0) {
            return written ? (int)written : -EPIPE;
        }

        int b = (unsigned char)buf[written];
        int sig = 0;
        if (p->termios.c_lflag & ISIG) {
            sig = signal_for_byte(&p->termios, b);
        }
        if (sig) {
            /* Real terminal line-discipline behavior (mirrors
             * kernel/vt.c's vt_input_push()): consumed immediately as a
             * signal, never queued as ordinary input data, so a job that
             * never calls read() at all (a CPU-bound loop, `sleep`) is
             * still interruptible. */
            const char *echo = echo_for_signal(sig);
            ring_push(&p->out, echo, strlen(echo));
            signal_send_pgrp((uint32_t)p->fg_pgid, sig);
            written++;
            continue;
        }

        if (p->in.count == PTY_BUF_SIZE) {
            if (p->slave_ends == 0) {
                return written ? (int)written : -EPIPE;
            }
            arch_enable_interrupts();
            pty_wait_ctx_t ctx = { .p = p };
            wait_queue_sleep(&p->in_write_wq, in_writable, &ctx);
            continue;
        }
        size_t n = ring_push(&p->in, buf + written, len - written);
        if (n == 0) {
            /* Ring genuinely full right this instant (raced with the check
             * above) -- loop back and wait properly instead of spinning. */
            continue;
        }
        written += n;
        wait_queue_wake_all(&p->in_read_wq);
    }
    return (int)written;
}

/* ---- Slave side --------------------------------------------------------
 * Mirrors drivers/tty.c's tty_read_canonical()/tty_read_raw() almost
 * exactly, just sourcing bytes from a ring buffer (blocking via
 * wait_queue_sleep()) instead of vt_input_getkey()'s KEY_* queue -- there is
 * no escape-sequence *decoding* step here because the master already writes
 * real bytes (arrow-key escape sequences included) directly. */

#define PTY_LINE_MAX 256

static int next_byte_blocking(pty_t *p)
{
    for (;;) {
        if (p->in.count > 0) {
            char c;
            ring_pop(&p->in, &c, 1);
            return (unsigned char)c;
        }
        if (p->master_ends == 0) {
            return -1; /* EOF */
        }
        arch_enable_interrupts();
        pty_wait_ctx_t ctx = { .p = p };
        wait_queue_sleep(&p->in_read_wq, in_readable, &ctx);
    }
}

static void pty_echo_char(pty_t *p, char c)
{
    if (p->termios.c_lflag & ECHO) {
        ring_push(&p->out, &c, 1);
    }
}

static void pty_echo_erase(pty_t *p)
{
    if (p->termios.c_lflag & (ECHO | ECHOE)) {
        ring_push(&p->out, "\b \b", 3);
    }
}

static int pty_read_canonical(pty_t *p, char *buf, size_t len)
{
    char line[PTY_LINE_MAX];
    size_t n = 0;

    for (;;) {
        int c = next_byte_blocking(p);
        if (c < 0) {
            /* EOF: master gone with nothing left buffered. Real UNIX
             * returns whatever partial line was assembled so far first,
             * then 0 on the next call -- but with no way to "come back
             * later" here beyond looping, returning what's assembled now
             * (possibly 0) matches a real tty-exit scenario closely enough
             * for a shell that's about to see this pty die anyway. */
            break;
        }
        if (c == (int)p->termios.c_cc[VEOF]) {
            if (n == 0) {
                return 0;
            }
            break;
        }
        if (c == '\n' || c == '\r') {
            pty_echo_char(p, '\n');
            line[n < sizeof(line) ? n : sizeof(line) - 1] = '\n';
            n = (n < sizeof(line)) ? n + 1 : sizeof(line);
            break;
        }
        if (c == (int)p->termios.c_cc[VERASE]) {
            if (n > 0) {
                n--;
                pty_echo_erase(p);
            }
            continue;
        }
        if (c == (int)p->termios.c_cc[VKILL]) {
            while (n > 0) {
                n--;
                pty_echo_erase(p);
            }
            if (p->termios.c_lflag & ECHOK) {
                pty_echo_char(p, '\n');
            }
            continue;
        }
        if (n < sizeof(line)) {
            line[n++] = (char)c;
            pty_echo_char(p, (char)c);
        }
    }

    size_t to_copy = (n < len) ? n : len;
    memcpy(buf, line, to_copy);
    return (int)to_copy;
}

static int pty_read_raw(pty_t *p, char *buf, size_t len)
{
    size_t i = 0;
    int first = next_byte_blocking(p);
    if (first < 0) {
        return 0; /* EOF */
    }
    buf[i++] = (char)first;
    pty_echo_char(p, (char)first);

    while (i < len && p->in.count > 0) {
        char c;
        ring_pop(&p->in, &c, 1);
        buf[i++] = c;
        pty_echo_char(p, c);
    }
    return (int)i;
}

int pty_slave_read(pty_t *p, char *buf, size_t len)
{
    if (!p || !buf) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (p->termios.c_lflag & ICANON) {
        return pty_read_canonical(p, buf, len);
    }
    return pty_read_raw(p, buf, len);
}

int pty_slave_write(pty_t *p, const char *buf, size_t len)
{
    if (!p || !buf) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (p->master_ends == 0) {
        return -EPIPE;
    }
    size_t written = 0;
    while (written < len) {
        if (p->out.count == PTY_BUF_SIZE) {
            if (p->master_ends == 0) {
                return written ? (int)written : -EPIPE;
            }
            arch_enable_interrupts();
            pty_wait_ctx_t ctx = { .p = p };
            wait_queue_sleep(&p->out_write_wq, out_writable, &ctx);
            continue;
        }
        size_t n = ring_push(&p->out, buf + written, len - written);
        if (n == 0) {
            continue;
        }
        written += n;
    }
    return (int)written;
}

/* ---- termios / job control / winsize ----------------------------------- */

int pty_get_termios(pty_t *p, struct termios *out)
{
    if (!p || !out) {
        return -EINVAL;
    }
    *out = p->termios;
    return 0;
}

int pty_set_termios(pty_t *p, const struct termios *in)
{
    if (!p || !in) {
        return -EINVAL;
    }
    p->termios = *in;
    return 0;
}

int pty_get_fg_pgid(pty_t *p)
{
    return p ? p->fg_pgid : 0;
}

void pty_set_fg_pgid(pty_t *p, int pgid)
{
    if (p) {
        p->fg_pgid = pgid;
    }
}

int pty_get_sid(pty_t *p)
{
    return p ? p->sid : 0;
}

void pty_set_sid(pty_t *p, int sid)
{
    if (p) {
        p->sid = sid;
    }
}

void pty_get_winsize(pty_t *p, unsigned short *rows, unsigned short *cols)
{
    if (!p) {
        return;
    }
    if (rows) *rows = p->rows;
    if (cols) *cols = p->cols;
}

void pty_set_winsize(pty_t *p, unsigned short rows, unsigned short cols)
{
    if (p) {
        p->rows = rows;
        p->cols = cols;
    }
}
