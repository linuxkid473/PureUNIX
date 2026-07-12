#ifndef PUREUNIX_VT_H
#define PUREUNIX_VT_H

#include <pureunix/input.h>
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

/* Raw input events (include/pureunix/input.h) -- a second event source fed
 * by the same keyboard/mouse drivers as vt_input_push(), for SDL2's event
 * pump (docs/sdl-port.md). Delivered only to the active VT's own queue,
 * exactly like vt_input_push(); a backgrounded VT's queue is never fed.
 *
 * vt_raw_input_push_key(): code is a KEY_* constant or ASCII value (see
 * include/pureunix/keyboard.h), independent of shift state -- e.g. the 'a'
 * key reports code == 'a' whether or not Shift/Caps Lock is held, so a
 * consumer can tell "which physical key" apart from "what character it
 * would produce", the same distinction SDL_Scancode vs SDL_Keycode makes.
 * pressed is 1 for a key-down, 0 for a key-up -- unlike vt_input_push(),
 * which only ever reports presses.
 *
 * vt_raw_input_push_mouse_motion(): dx/dy is relative motion since the
 * last call; the kernel maintains one global absolute pointer position
 * (clamped to the framebuffer's pixel dimensions) and reports it back in
 * the pushed event's x/y fields.
 *
 * vt_raw_input_push_mouse_button(): code is a PU_MOUSE_BTN_* constant. */
void vt_raw_input_push_key(int code, int pressed);
void vt_raw_input_push_mouse_motion(int dx, int dy);
void vt_raw_input_push_mouse_button(int code, int pressed);
/* Non-blocking pop from VT vt_id's raw queue. Returns true and fills *out
 * if an event was available, false (leaving *out untouched) otherwise --
 * SDL's event pump is poll-driven (called once per frame), never blocking,
 * so there is no vt_raw_input_get() blocking counterpart. */
bool vt_raw_input_try_get(int vt_id, pu_input_event_t *out);

/* Text mode / graphics mode (SYS_SET_GRAPHICS_MODE, docs/sdl-port.md) --
 * while a VT is in graphics mode, drivers/vga.c's own repaint entry points
 * (vga_bind_active()/vga_console_repaint()) leave the framebuffer alone
 * for that VT instead of overwriting an SDL app's rendering with console
 * text, and console *writes* to that VT (vt_putc()/vt_write()) still
 * update its saved cell buffer but skip the real hardware draw -- exactly
 * mirroring how those functions already behave for a merely-backgrounded
 * VT, just gated on this flag instead of (or in addition to) "is this the
 * active VT". Leaving graphics mode forces a full text repaint if the VT
 * is still active, so the console reappears exactly as it would have if
 * the SDL app had never run. */
void vt_set_graphics_mode(int vt_id, bool enable);
bool vt_is_graphics_mode(int vt_id);

/* Broadcasts SIGWINCH to every VT's foreground process group. The console
 * grid (drivers/vga.c's vga_cols/vga_rows) is one hardware-wide size shared
 * by every VT, so a resize (currently only TIOCSFONT — see SYS_IOCTL,
 * arch/i386/syscall.c) changes every VT's terminal dimensions at once, not
 * just the calling task's own — real Linux only signals the VT that
 * actually resized, but there's only ever one real size here to signal
 * about. pgid <= 0 (no session has claimed that VT yet) is silently
 * skipped, same as vt_input_push()'s signal path. */
void vt_signal_resize(void);

#endif
