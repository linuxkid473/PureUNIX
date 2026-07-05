#ifndef PUREUNIX_IOCTL_H
#define PUREUNIX_IOCTL_H

/* ioctl() request codes. PureUNIX's console is a fixed 80x25 VGA text grid
 * (drivers/vga.c) with no resize event ever possible, so TIOCGWINSZ is the
 * one request worth having: it lets a full-screen program (an editor, a
 * pager) size its viewport instead of assuming 80x25 outright. See
 * SYS_IOCTL in arch/i386/syscall.c. */
#define TIOCGWINSZ 1

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel; /* unused: PureUNIX has no pixel-accurate console geometry */
    unsigned short ws_ypixel; /* unused, same reason */
};

#endif
