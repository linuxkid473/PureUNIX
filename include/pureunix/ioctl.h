#ifndef PUREUNIX_IOCTL_H
#define PUREUNIX_IOCTL_H

/* ioctl() request codes. PureUNIX's console grid is sized to the detected
 * framebuffer resolution and current font scale (drivers/vga.c), not a
 * fixed 80x25, and can change size at runtime via TIOCSFONT — so
 * TIOCGWINSZ is what lets a full-screen program (an editor, a pager) size
 * its viewport instead of assuming any fixed dimensions. See SYS_IOCTL in
 * arch/i386/syscall.c. */
#define TIOCGWINSZ 1

/* Sets the console's font scale (see drivers/font.c's font_set_scale()):
 * argp is an `int *` in [FONT_SCALE_MIN, FONT_SCALE_MAX] (drivers/font.h).
 * Resizes the console grid and repaints. Fails with -EINVAL if argp is
 * null, out of range, or the console is in legacy 80x25 VGA text mode (no
 * framebuffer, so no adjustable glyph size). See the `font` command
 * (user/font.c). */
#define TIOCSFONT 2

/* Virtual terminal switching (include/pureunix/vt.h) — the `tty` command
 * (user/tty.c) uses these so it goes through the exact same kernel
 * VT-switching path (vt_switch()) as the Alt+F<n> keyboard shortcut, not a
 * separate implementation. VT numbers here are 1-based (VT_GETACTIVE/
 * VT_ACTIVATE's argp is a `1..NUM_VTS` VT number, matching the vtN naming
 * users see — vt_switch() itself is 0-based internally). */
#define VT_GETACTIVE 3 /* argp: int * (out) — the caller's current VT number */
#define VT_ACTIVATE  4 /* argp: const int * (in) — VT number to switch to */

/* Foreground process group of the VT `fd` names — the job-control-aware
 * shell's tcgetpgrp()/tcsetpgrp() (POSIX), used by BusyBox ash to know
 * (and move) which job currently owns keyboard-generated signals
 * (Ctrl+C/Ctrl+Z/Ctrl+\) and read access on that terminal. See
 * kernel/vt.c's vt_get_fg_pgid()/vt_set_fg_pgid() and SYS_IOCTL in
 * arch/i386/syscall.c. */
#define TIOCGPGRP 5 /* argp: pid_t * (out) */
#define TIOCSPGRP 6 /* argp: const pid_t * (in) */

/* PTY-only requests (include/pureunix/pty.h, kernel/pty.c) — a physical
 * VT's TIOCGWINSZ already reflects the one shared hardware grid and can't
 * be resized per-fd, but a pty's size is whatever its master (PUTerm) says
 * it is, and there's no controlling-terminal auto-assignment heuristic in
 * this kernel to grant one implicitly, so a real ioctl exists for it. */
#define TIOCSWINSZ 7 /* argp: const struct winsize * (in) — PTY fds only */
#define TIOCSCTTY  8 /* argp: unused (pass NULL) — makes this fd's pty the
                      * caller's controlling terminal; caller must be a
                      * session leader (sid == own pid), real POSIX rule */

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel; /* unused: PureUNIX has no pixel-accurate console geometry */
    unsigned short ws_ypixel; /* unused, same reason */
};

#endif
