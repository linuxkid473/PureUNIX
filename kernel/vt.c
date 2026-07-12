/* kernel/vt.c — Virtual Terminal subsystem: NUM_VTS independent consoles,
 * one active at a time. See include/pureunix/vt.h for the design rationale.
 *
 * Deliberately thin: drivers/vga.c already knows how to own N independent
 * console_t buffers and switch which one drives real hardware; this file's
 * job is policy — which VT is active, per-VT termios, per-VT keyboard input
 * queues, and vt_switch() as the single kernel-wide VT-switching path. */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/keyboard.h>
#include <pureunix/signal.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/vga.h>
#include <pureunix/vt.h>
#include <pureunix/wait.h>

#define VT_KEYBUF_SIZE 128

typedef struct vt {
    console_t *console;
    struct termios termios;
    int keybuf[VT_KEYBUF_SIZE];
    volatile uint32_t key_head;
    volatile uint32_t key_tail;
    /* Woken by vt_input_push() whenever this VT gains a queued key —
     * see vt_input_getkey() below. */
    wait_queue_t key_wq;
    /* Controlling-terminal/job-control state — see vt_claim_session()/
     * vt_get_fg_pgid()/vt_set_fg_pgid() and include/pureunix/vt.h. 0
     * until a session claims this VT (0 is never a valid pgid/sid, real
     * ids start at 1). */
    int owner_sid;
    int fg_pgid;
} vt_t;

static vt_t vts[NUM_VTS];
static int active_vt;

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

void vt_init(void)
{
    /* VT1 (index 0) claims vga.c's boot console directly — it already holds
     * every boot message printed so far; a freshly reset one would throw
     * that away the instant vt_switch()/vga_bind_active() next repainted. */
    vts[0].console = vga_console(0);
    termios_defaults(&vts[0].termios);

    for (int i = 1; i < NUM_VTS; ++i) {
        vts[i].console = vga_console(i);
        /* The console pool is BSS (zero-initialized) -- without this, a
         * fresh console's color defaults to black-on-black (0x00), not
         * "unset"/blank, so every character its owning shell ever prints
         * would be genuinely invisible even though it's being drawn
         * correctly. Must run before vga_console_reset(), which blanks
         * every cell using whatever color is already set (matching
         * vga_init()'s own color-then-reset order for console 0/VT1). */
        vga_console_set_color(vts[i].console, vga_default_color());
        vga_console_reset(vts[i].console);
        termios_defaults(&vts[i].termios);
    }
    active_vt = 0;
}

static bool valid_vt(int n)
{
    return n >= 0 && n < NUM_VTS;
}

void vt_switch(int n)
{
    if (!valid_vt(n) || n == active_vt) {
        return;
    }
    active_vt = n;
    vga_bind_active(vts[n].console);
}

int vt_active_id(void)
{
    return active_vt;
}

/* Same KEY_* -> raw-control-byte normalization drivers/tty.c's
 * key_to_byte() does for VERASE/VKILL/data characters, but only the
 * three bytes vt_input_push() itself needs to recognize as *signal*
 * characters — real canonical-mode line editing (echo, backspace,
 * line-kill) stays entirely in drivers/tty.c, which is a strictly higher
 * layer than this one and must never be called from here. */
static int key_to_signal_byte(int key)
{
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

void vt_input_push(int key)
{
    if (key == 0 /* KEY_NONE */) {
        return;
    }
    vt_t *vt = &vts[active_vt];

    /* Real terminal line-discipline behavior: Ctrl+C/Ctrl+Z/Ctrl+\ are
     * consumed by the tty layer itself the instant they arrive — not
     * queued as ordinary input data — and reach the *foreground process
     * group*, regardless of whether anything is even blocked in read()
     * right now (a job that never reads stdin at all, e.g. a CPU-bound
     * loop or `sleep`, must still be interruptible by Ctrl+C — coupling
     * this to drivers/tty.c's read() path the way an earlier version of
     * this code did would silently fail to interrupt exactly those
     * cases). Respects ISIG exactly like drivers/tty.c's own read()
     * paths do for the same three characters when ISIG is off (e.g. a
     * raw-mode full-screen program that wants Ctrl+C delivered as
     * literal data) — see docs/process-management.md. */
    if (vt->termios.c_lflag & ISIG) {
        int b = key_to_signal_byte(key);
        int sig = 0;
        const char *echo = NULL;
        if (b == (int)vt->termios.c_cc[VINTR]) {
            sig = SIGINT;
            echo = "^C\n";
        } else if (b == (int)vt->termios.c_cc[VQUIT]) {
            sig = SIGQUIT;
            echo = "^\\\n";
        } else if (b == (int)vt->termios.c_cc[VSUSP]) {
            sig = SIGTSTP;
            echo = "^Z\n";
        }
        if (sig) {
            /* Called from the keyboard IRQ handler (drivers/keyboard.c /
             * drivers/hid.c) — vt_write()/signal_send_pgrp() only ever
             * touch console buffers / task_t fields directly, never block
             * or context-switch, so this is IRQ-context safe the same way
             * wait_queue_wake_one() below is. */
            vt_write(active_vt, echo, strlen(echo));
            signal_send_pgrp((uint32_t)vt->fg_pgid, sig);
            return;
        }
    }

    uint32_t next = (vt->key_head + 1) % VT_KEYBUF_SIZE;
    if (next != vt->key_tail) {
        vt->keybuf[vt->key_head] = key;
        vt->key_head = next;
        /* wait_queue_wake_one() is IRQ-context safe (see
         * include/pureunix/wait.h). One key pushed can only satisfy one
         * blocked reader's "queue is non-empty" predicate at a time. */
        wait_queue_wake_one(&vt->key_wq);
    }
}

int vt_input_try_getkey(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return 0;
    }
    vt_t *vt = &vts[vt_id];
    if (vt->key_head == vt->key_tail) {
        return 0;
    }
    int key = vt->keybuf[vt->key_tail];
    vt->key_tail = (vt->key_tail + 1) % VT_KEYBUF_SIZE;
    return key;
}

typedef struct {
    int vt_id;
    int key;
} getkey_ctx_t;

static bool getkey_ready(void *ctx)
{
    getkey_ctx_t *c = (getkey_ctx_t *)ctx;
    return (c->key = vt_input_try_getkey(c->vt_id)) != 0;
}

int vt_input_getkey(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return 0;
    }
    getkey_ctx_t ctx = { .vt_id = vt_id, .key = 0 };
    /* See include/pureunix/wait.h's invariant: callers reached through
     * int $0x80 must already have called arch_enable_interrupts() —
     * drivers/tty.c's tty_read() does, before ever reaching here. */
    wait_queue_sleep(&vts[vt_id].key_wq, getkey_ready, &ctx);
    return ctx.key;
}

void vt_putc(int vt_id, char c)
{
    if (!valid_vt(vt_id)) {
        vga_putc(c);
        return;
    }
    vga_putc_vt(vts[vt_id].console, c);
}

void vt_write(int vt_id, const char *data, size_t len)
{
    if (!valid_vt(vt_id)) {
        vga_write_len(data, len);
        return;
    }
    vga_write_len_vt(vts[vt_id].console, data, len);
}

void vt_get_size(size_t *rows, size_t *cols)
{
    vga_get_size(rows, cols);
}

int vt_get_termios(int vt_id, struct termios *out)
{
    if (!out) {
        return -EINVAL;
    }
    if (!valid_vt(vt_id)) {
        vt_id = active_vt;
    }
    *out = vts[vt_id].termios;
    return 0;
}

int vt_set_termios(int vt_id, const struct termios *in)
{
    if (!in) {
        return -EINVAL;
    }
    if (!valid_vt(vt_id)) {
        vt_id = active_vt;
    }
    vts[vt_id].termios = *in;
    return 0;
}

void vt_scroll_view(int vt_id, int delta)
{
    if (!valid_vt(vt_id)) {
        return;
    }
    vga_console_scroll_view(vts[vt_id].console, delta);
}

void vt_claim_session(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return;
    }
    task_t *t = task_current();
    if (!t) {
        return;
    }
    t->sid = t->id;
    t->pgid = t->id;
    vts[vt_id].owner_sid = (int)t->id;
    vts[vt_id].fg_pgid = (int)t->id;
}

int vt_get_fg_pgid(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return 0;
    }
    return vts[vt_id].fg_pgid;
}

void vt_set_fg_pgid(int vt_id, int pgid)
{
    if (!valid_vt(vt_id)) {
        return;
    }
    vts[vt_id].fg_pgid = pgid;
}

void vt_signal_resize(void)
{
    for (int i = 0; i < NUM_VTS; ++i) {
        if (vts[i].fg_pgid > 0) {
            signal_send_pgrp((uint32_t)vts[i].fg_pgid, SIGWINCH);
        }
    }
}
