/* kernel/vt.c — Virtual Terminal subsystem: NUM_VTS independent consoles,
 * one active at a time. See include/pureunix/vt.h for the design rationale.
 *
 * Deliberately thin: drivers/vga.c already knows how to own N independent
 * console_t buffers and switch which one drives real hardware; this file's
 * job is policy — which VT is active, per-VT termios, per-VT keyboard input
 * queues, and vt_switch() as the single kernel-wide VT-switching path. */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/vga.h>
#include <pureunix/vt.h>

#define VT_KEYBUF_SIZE 128

typedef struct vt {
    console_t *console;
    struct termios termios;
    int keybuf[VT_KEYBUF_SIZE];
    volatile uint32_t key_head;
    volatile uint32_t key_tail;
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

void vt_input_push(int key)
{
    if (key == 0 /* KEY_NONE */) {
        return;
    }
    vt_t *vt = &vts[active_vt];
    uint32_t next = (vt->key_head + 1) % VT_KEYBUF_SIZE;
    if (next != vt->key_tail) {
        vt->keybuf[vt->key_head] = key;
        vt->key_head = next;
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

int vt_input_getkey(int vt_id)
{
    int key;
    while ((key = vt_input_try_getkey(vt_id)) == 0) {
        /* Yield first so any other ready task (a background VT's shell, a
         * long-lived process on it, ...) gets a turn while this task has
         * nothing useful to do; if nothing else is ready, task_yield() is a
         * no-op and the halt below just waits for the next interrupt (timer
         * tick, keyboard, ...) instead of spinning. */
        task_yield();
        arch_halt();
    }
    return key;
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
