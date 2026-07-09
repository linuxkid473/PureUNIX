#ifndef VI_COMPAT_SYS_IOCTL_H
#define VI_COMPAT_SYS_IOCTL_H

/* TIOCGWINSZ/struct winsize are already mirrored in libpure.h for user
 * programs (see compat/termios.h's header comment for why); this just adds
 * a POSIX-named wrapper around pu_ioctl() so term.c can call ioctl() like
 * upstream Neatvi expects. */
#include "libpure.h"

int ioctl(int fd, int request, void *argp);

#endif
