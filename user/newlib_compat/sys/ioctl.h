/* Override for glibc's <sys/ioctl.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). Unlike the other
 * compat headers in this directory, PureUNIX genuinely has a working
 * ioctl() — SYS_IOCTL (docs/syscalls.md), implemented in
 * user/newlib_syscalls.c — so this isn't just a stub: these request codes
 * and struct winsize match include/pureunix/ioctl.h exactly (the only
 * requests PureUNIX's console supports; anything else fails with -EINVAL
 * at runtime, same as the real syscall).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_IOCTL_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_IOCTL_H

#define TIOCGWINSZ 1
#define TIOCSFONT  2
/* VT_GETACTIVE/VT_ACTIVATE (include/pureunix/ioctl.h's request codes 3/4)
 * are deliberately *not* mirrored here: nothing in the newlib/BusyBox
 * toolchain uses them (user/tty.c, the only caller, is a libpure — not
 * newlib — program and gets them from user/libpure.h instead), and
 * BusyBox's own libbb/get_console.c locally defines an identifier named
 * VT_ACTIVATE for a completely different, real Linux ioctl request
 * number (0x5606) — #define-ing a small integer under the same name here
 * would silently corrupt that unrelated declaration (`VT_ACTIVATE = ...`
 * becomes `4 = ...`, a syntax error) the instant anything in this
 * directory's search path is visible to it. Confirmed as the actual
 * cause of a real busybox rebuild failure, not a hypothetical. */
#define TIOCGPGRP 5
#define TIOCSPGRP 6
/* PTY-only requests (include/pureunix/pty.h, kernel/pty.c) — see that
 * header's own comment. PUTerm (docs/pude.md) is the only newlib-linked
 * consumer of either. */
#define TIOCSWINSZ 7
#define TIOCSCTTY  8

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, int request, ...);

#endif
