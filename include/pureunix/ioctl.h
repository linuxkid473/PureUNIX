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

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel; /* unused: PureUNIX has no pixel-accurate console geometry */
    unsigned short ws_ypixel; /* unused, same reason */
};

#endif
