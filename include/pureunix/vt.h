#ifndef PUREUNIX_VT_H
#define PUREUNIX_VT_H

#include <pureunix/termios.h>
#include <pureunix/types.h>

/* Virtual Terminal subsystem: NUM_VTS independent consoles (drivers/vga.c's
 * console_t: text buffer, cursor, scrollback), each with its own termios
 * and keyboard input queue, exactly one of which is "active" (on screen,
 * receiving keystrokes) at a time. A task belongs to a VT via its own
 * task_t.vt_id (include/pureunix/task.h), inherited by every child it
 * creates — so a shell and everything it launches (ping, seq, make, ...)
 * all read/write the same VT, and switching away from it never touches
 * those tasks: they keep running, they just stop being able to read a key
 * that will never arrive until their VT is active again (see vt_switch()).
 *
 * vt_switch() is the *only* place that changes which VT is active — both
 * Alt+F<n> (drivers/keyboard.c, drivers/hid.c) and the `tty N` command
 * (VT_ACTIVATE via SYS_IOCTL, arch/i386/syscall.c) call it and nothing
 * else, so there is exactly one kernel VT-switching code path. */

#define NUM_VTS 6

void vt_init(void);

/* n is 0-based (0..NUM_VTS-1), i.e. VT1 == 0. Redraws the newly active
 * console from its own saved buffer (drivers/vga.c's vga_bind_active()) and
 * makes it the target of subsequent keyboard input. No-op if n is already
 * active or out of range. */
void vt_switch(int n);
int vt_active_id(void);

/* Delivers one key event to whichever VT is currently active — called by
 * the keyboard drivers for every key that isn't consumed as an Alt+F<n>
 * switch request, so a keystroke can only ever reach the VT actually on
 * screen (include/pureunix/keyboard.h's KEY_* constants, or a printable
 * byte). */
void vt_input_push(int key);
/* Reads (or peeks) VT vt_id's own input queue, regardless of whether it's
 * currently active — a backgrounded VT's queue is simply never fed (see
 * vt_input_push()), so a blocking read on it blocks until that VT becomes
 * active again, exactly like a real Linux VT. */
int vt_input_try_getkey(int vt_id);
/* Blocks (yielding to other tasks — see kernel/task.c's task_yield() and
 * docs/scheduler.md) until VT vt_id's queue has a key. */
int vt_input_getkey(int vt_id);

/* Per-VT console output. Updates VT vt_id's own saved buffer; only touches
 * real hardware if vt_id happens to be the active VT. Used by the fd 0/1/2
 * default console binding (SYS_WRITE, arch/i386/syscall.c) so a
 * backgrounded VT's output never bleeds onto the foreground screen. */
void vt_putc(int vt_id, char c);
void vt_write(int vt_id, const char *data, size_t len);

/* The physical console grid size — the same for every VT (one display). */
void vt_get_size(size_t *rows, size_t *cols);

/* Per-VT termios, used by drivers/tty.c. */
int vt_get_termios(int vt_id, struct termios *out);
int vt_set_termios(int vt_id, const struct termios *in);

/* Scrollback view — Shift+PageUp/PageDown (drivers/keyboard.c/hid.c) call
 * this on whichever VT is active. */
void vt_scroll_view(int vt_id, int delta);

/* Makes the calling task a session leader (sid == pgid == its own pid —
 * see include/pureunix/task.h) and VT vt_id's controlling-terminal owner,
 * with itself as that VT's initial foreground process group. Called
 * exactly once per VT, by whichever task first claims it as its own
 * (kernel/main.c: main_task for VT1, each vt_session_main() task for
 * VT2..NUM_VTS) — see docs/process-management.md. */
void vt_claim_session(int vt_id);

/* VT vt_id's current foreground process group — the only pgid whose
 * member processes may receive a keyboard-generated signal (Ctrl+C/
 * Ctrl+Z/Ctrl+\) from that VT, and (once M6 wires it up) the only pgid
 * whose processes may read from that VT without SIGTTIN-equivalent
 * blocking. 0 (never a valid pgid) if vt_id is out of range or hasn't
 * been claimed by a session yet. */
int vt_get_fg_pgid(int vt_id);
/* Sets VT vt_id's foreground process group — the kernel-side backing for
 * tcsetpgrp() (TIOCSPGRP via SYS_IOCTL, arch/i386/syscall.c), which is
 * how job-control-aware shells (BusyBox ash) move a job into the
 * foreground. No membership/session validation is performed here (kept
 * intentionally simple — see docs/process-management.md's scope notes);
 * arch/i386/syscall.c's ioctl handler is what applies POSIX's caller-
 * must-share-this-controlling-terminal's-session check before calling
 * this. */
void vt_set_fg_pgid(int vt_id, int pgid);

#endif
