/* Override for glibc's <sys/ioctl.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). Unlike the other
 * compat headers in this directory, PureUNIX genuinely has a working
 * ioctl() — SYS_IOCTL (docs/syscalls.md), implemented in
 * user/newlib_syscalls.c — so this isn't just a stub: TIOCGWINSZ/TIOCSFONT
 * and struct winsize match include/pureunix/ioctl.h exactly (the only
 * requests PureUNIX's console supports; anything else fails with -EINVAL
 * at runtime, same as the real syscall).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_IOCTL_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_IOCTL_H

#define TIOCGWINSZ 1
#define TIOCSFONT  2

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, int request, ...);

#endif
