/* kernel/vt.c — Virtual Terminal subsystem: NUM_VTS independent consoles,
 * one active at a time. See include/pureunix/vt.h for the design rationale.
 *
 * Deliberately thin: drivers/vga.c already knows how to own N independent
 * console_t buffers and switch which one drives real hardware; this file's
 * job is policy — which VT is active, per-VT termios, per-VT keyboard input
 * queues, and vt_switch() as the single kernel-wide VT-switching path. */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/framebuffer.h>
#include <pureunix/keyboard.h>
#include <pureunix/signal.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/vga.h>
#include <pureunix/vt.h>
#include <pureunix/wait.h>

#define VT_KEYBUF_SIZE 128
/* Deeper than VT_KEYBUF_SIZE: mouse motion can generate several events per
 * PIT tick while the pointer is moving, and unlike the ASCII queue nothing
 * here collapses repeats -- an SDL app polls this queue roughly once per
 * frame, not once per keystroke, so it needs more slack against bursts. */
#define VT_RAWBUF_SIZE 256

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

    /* Raw input queue (include/pureunix/input.h) -- see vt.h's comment on
     * vt_raw_input_push_key()/vt_raw_input_try_get(). A plain ring buffer,
     * not wait-queue-backed like keybuf above: consumers poll it (SDL's
     * event pump runs once per frame), they never block waiting for it. */
    pu_input_event_t rawbuf[VT_RAWBUF_SIZE];
    volatile uint32_t raw_head;
    volatile uint32_t raw_tail;

    /* Which task's SYS_SET_GRAPHICS_MODE(1) call put this VT into graphics
     * mode -- see vt_get_graphics_owner()'s declaration (include/pureunix/
     * vt.h) for why this matters once a graphics-mode app can have real
     * children (PUTerm, docs/pude.md). 0 (never a valid task id) when not
     * in graphics mode or no owner recorded. */
    uint32_t graphics_owner;

    /* Stack of previous graphics_owner values, one push per *nested*
     * SYS_SET_GRAPHICS_MODE(1) call -- e.g. Chocolate Doom, itself a real
     * SDL app, calling SET_GRAPHICS_MODE(1) from a shell running inside
     * PUTerm (also a graphics-mode owner of the same vt_id). Without this,
     * disabling graphics mode when the *nested* app exits/is killed wiped
     * out graphics_owner and physically turned graphics mode off, even
     * though the outer WM (PUTerm) was still alive and still wanted it on
     * -- confirmed as the real cause of PUTerm permanently freezing (every
     * subsequent SYS_FB_BLIT silently no-op'd by vt_is_graphics_mode())
     * after Ctrl+S killed a nested Doom (docs/pude.md). */
    uint32_t graphics_owner_stack[8];
    int graphics_owner_depth;
} vt_t;

static vt_t vts[NUM_VTS];
static int active_vt;

/* One global pointer position, clamped to the framebuffer's pixel bounds --
 * mice don't belong to a particular VT the way a keyboard's *destination*
 * does, but events are still only ever delivered to whichever VT is active
 * (same as vt_input_push()), so a single shared position is enough: it's
 * meaningless while no VT is actually consuming it. */
static int32_t mouse_x, mouse_y;

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

    if (vga_console_is_graphics_mode(vt->console)) {
        /* Graphics mode is checked *before* ISIG interception below —
         * a graphics-mode app (SDL games, and now PUTerm, docs/pude.md)
         * owns this physical keystroke via the raw input queue instead of
         * the ASCII queue's line discipline, and gets to decide for
         * itself what Ctrl+C/Z/\ mean (PUTerm forwards them to its own
         * pty's *own* independent ISIG interception, kernel/pty.c's
         * pty_master_write() — exactly mirroring this function). Before
         * this ordering existed, VT1's own fg_pgid (set to the graphics
         * app's pgid by the shell's ordinary job control when it launched
         * that app as a foreground job) intercepted the *same* physical
         * Ctrl+C a *second* time and killed the app outright via SIGINT's
         * default action — confirmed as the exact, reproducible cause of
         * PUTerm's window vanishing the instant Ctrl+C was pressed on a
         * job running *inside* it: the outer VT's own top-level line
         * discipline was killing PUTerm itself before PUTerm's own pty
         * ever saw the keystroke. Real terminal emulators have exactly
         * this property too — the window system never independently
         * signals a terminal emulator's process on Ctrl+C; only the
         * terminal's own pty's line discipline does, targeting whatever
         * job is in *its* foreground.
         *
         * Ctrl+S is repurposed, only while this VT is graphics-mode, as
         * an unconditional emergency kill switch — deliberately *not*
         * gated by ISIG the way Ctrl+C/Z/\ are: those now rely on the app
         * handling them itself (true of every SDL/chocolate-doom program,
         * which never touches ISIG, and now of PUTerm, which does handle
         * them via its own pty), but this hotkey exists precisely so a
         * wedged/misbehaving graphics app can never leave the console
         * stuck. SIGKILL (not SIGTERM/SIGINT) because it cannot be
         * blocked, ignored, or handled — "recursively close" every
         * process in the foreground group, guaranteed. kernel/signal.c's
         * SIGKILL handling already forces graphics_mode back off (only
         * for the actual owner — vt_get_graphics_owner() — see that
         * function's own comment) and repaints as part of tearing the
         * target down, so this one keypress alone returns the VT straight
         * back to its ash shell, synchronously, with no dependence on the
         * app's own (possibly hung) cleanup path. Outside graphics mode
         * this falls through unchanged to the ordinary ASCII queue below,
         * so editor.c's existing KEY_CTRL_S ("save") meaning is completely
         * unaffected. */
        if (key == KEY_CTRL_S) {
            /* Target the *actual current graphics owner*'s process group,
             * not vt->fg_pgid -- those two can genuinely differ now that a
             * graphics-mode app can itself have descendants that become a
             * *different* graphics-mode app (PUTerm running a real shell,
             * docs/pude.md, which can launch e.g. Chocolate Doom). vt-
             * >fg_pgid reflects whatever the *outer* shell's ordinary job
             * control set when it first launched the top-level app (e.g.
             * PUTerm's own pgid) and is never updated again by anything
             * nested underneath — using it here would SIGKILL the
             * top-level app (PUTerm itself) while leaving the actually-
             * wedged nested app (Doom) completely untouched and still
             * rendering, defeating the entire point of this emergency
             * kill switch. Confirmed as a real, reproducible bug this way:
             * Ctrl+S while a nested Doom was running silently killed
             * PUTerm instead. Falls back to vt->fg_pgid only if the
             * recorded owner task somehow no longer exists (shouldn't
             * normally happen — belt and suspenders, not worse than the
             * previous behavior). */
            task_t *owner = task_find((uint32_t)vts[active_vt].graphics_owner);
            uint32_t target_pgid = owner ? owner->pgid : (uint32_t)vt->fg_pgid;
            vt_write(active_vt, "\n^S: killing graphics app\n", 27);
            signal_send_pgrp(target_pgid, SIGKILL);
            return;
        }
        /* An SDL app owns this VT via the raw input queue (see
         * vt_raw_input_push_key() below) instead of ordinary ASCII reads.
         * Without this, every key typed during a graphics-mode session
         * (movement keys during a game, etc.) would still pile up
         * unread in this canonical queue, invisible until the app exits
         * and the underlying shell resumes reading its tty — at which
         * point the entire backlog replays at once as garbage input,
         * corrupting whatever command the user types next. Matches the
         * same graphics_mode gate drivers/vga.c already applies to
         * output. */
        return;
    }

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
     * literal data) — see docs/process-management.md. Only reached
     * outside graphics mode — see that check above for why. */
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

/* Common enqueue helper for all three vt_raw_input_push_*() entry points
 * below -- IRQ-context safe (plain ring buffer, no blocking), matching
 * vt_input_push()'s own safety requirement (called from the same keyboard/
 * mouse IRQ handlers). Silently drops the event if the active VT's raw
 * queue is full: an SDL app that stalls badly enough to fill 256 slots
 * loses old motion/key events rather than the kernel blocking or growing
 * unbounded, the same "drop, don't block" policy real evdev queues use
 * under overflow. */
static void raw_push(pu_input_event_t ev)
{
    vt_t *vt = &vts[active_vt];
    uint32_t next = (vt->raw_head + 1) % VT_RAWBUF_SIZE;
    if (next == vt->raw_tail) {
        return;
    }
    vt->rawbuf[vt->raw_head] = ev;
    vt->raw_head = next;
}

void vt_raw_input_push_key(int code, int pressed)
{
    pu_input_event_t ev = { .type = PU_INPUT_KEY, .code = code, .value = pressed };
    raw_push(ev);
}

void vt_raw_input_push_mouse_motion(int dx, int dy)
{
    const fb_info_t *fb = fb_get_info();
    int32_t max_x = fb->present && fb->width > 0 ? (int32_t)fb->width - 1 : 0;
    int32_t max_y = fb->present && fb->height > 0 ? (int32_t)fb->height - 1 : 0;

    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > max_x) mouse_x = max_x;
    if (mouse_y > max_y) mouse_y = max_y;

    pu_input_event_t ev = {
        .type = PU_INPUT_MOUSE_MOTION, .dx = dx, .dy = dy, .x = mouse_x, .y = mouse_y,
    };
    raw_push(ev);
}

void vt_raw_input_push_mouse_button(int code, int pressed)
{
    pu_input_event_t ev = {
        .type = PU_INPUT_MOUSE_BUTTON, .code = code, .value = pressed, .x = mouse_x, .y = mouse_y,
    };
    raw_push(ev);
}

bool vt_raw_input_try_get(int vt_id, pu_input_event_t *out)
{
    if (!valid_vt(vt_id) || !out) {
        return false;
    }
    vt_t *vt = &vts[vt_id];
    if (vt->raw_head == vt->raw_tail) {
        return false;
    }
    *out = vt->rawbuf[vt->raw_tail];
    vt->raw_tail = (vt->raw_tail + 1) % VT_RAWBUF_SIZE;
    return true;
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

void vt_set_graphics_mode(int vt_id, bool enable)
{
    if (!valid_vt(vt_id)) {
        return;
    }
    vt_t *vt = &vts[vt_id];
    if (enable) {
        /* Entering graphics mode: discard whatever raw input events already
         * queued *before* this app started polling for them -- the raw
         * queue (unlike the ASCII queue, which vt_input_push() already
         * stops filling once graphics_mode is set) is fed unconditionally
         * by the keyboard IRQ regardless of graphics mode, so keystrokes
         * used to *launch* this program (e.g. typing "pude\n" at the shell
         * prompt) sit queued and get silently replayed as live input the
         * instant SDL_PollEvent()/pu_input_poll() first drains them --
         * confirmed as a real, reproducible bug this way (docs/pude.md):
         * launching PUTerm made it immediately "type" its own launch
         * command into the freshly-forked shell. Matches this VT's own
         * "graphics mode starts with a clean slate" invariant, just
         * extended to the second input queue. */
        vts[vt_id].raw_head = vts[vt_id].raw_tail = 0;
        /* Push whatever owner (possibly 0, meaning "none" -- this is the
         * outermost enable) was already recorded, so a later disable can
         * tell a nested app's exit from the outermost one's. */
        if (vt->graphics_owner_depth < 8) {
            vt->graphics_owner_stack[vt->graphics_owner_depth++] =
                vt->graphics_owner;
        }
        task_t *t = task_current();
        vt->graphics_owner = t ? t->id : 0;
        vga_console_set_graphics_mode(vt->console, true);
    } else {
        uint32_t prev_owner = 0;
        bool had_prev = false;
        if (vt->graphics_owner_depth > 0) {
            prev_owner = vt->graphics_owner_stack[--vt->graphics_owner_depth];
            had_prev = true;
        }
        vt->graphics_owner = prev_owner;
        if (had_prev && prev_owner != 0) {
            /* A nested graphics app is exiting, but an outer owner (e.g.
             * PUTerm) is still alive and still wants graphics mode on --
             * leave the hardware/console graphics_mode flag untouched so
             * the outer owner's own SYS_FB_BLIT calls keep succeeding
             * instead of silently no-op'ing forever (see vt_is_graphics_
             * mode() gate in arch/i386/syscall.c's SYS_FB_BLIT handler).
             * The outer owner's own render loop redraws the next frame;
             * nothing here needs to repaint anything. */
            return;
        }
        vga_console_set_graphics_mode(vt->console, false);
    }
    if (!enable && vt_id == active_vt) {
        /* Leaving graphics mode while still on screen: force the console
         * back into view immediately rather than waiting for whatever
         * next happens to write to it -- see include/pureunix/vt.h's
         * "clean exit" comment. vga_bind_active() would no-op here (it
         * only repaints on an actual g_active change), so this calls the
         * repaint directly, same as vga_bind_active() does internally. */
        vga_console_repaint(vt->console);
    }
}

bool vt_is_graphics_mode(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return false;
    }
    return vga_console_is_graphics_mode(vts[vt_id].console);
}

uint32_t vt_get_graphics_owner(int vt_id)
{
    if (!valid_vt(vt_id)) {
        return 0;
    }
    return vts[vt_id].graphics_owner;
}

void vt_signal_resize(void)
{
    for (int i = 0; i < NUM_VTS; ++i) {
        if (vts[i].fg_pgid > 0) {
            signal_send_pgrp((uint32_t)vts[i].fg_pgid, SIGWINCH);
        }
    }
}
