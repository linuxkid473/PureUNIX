#ifndef VI_COMPAT_TERMIOS_H
#define VI_COMPAT_TERMIOS_H

/* struct termios / the c_lflag bit macros / TCSANOW are already mirrored in
 * libpure.h for user programs (see docs/developer-guide.md's "Adding a
 * System Call" — user code can't include kernel headers, so libpure.h
 * duplicates the constants); this just adds POSIX-named wrappers around
 * pu_tcgetattr()/pu_tcsetattr() so term.c can call tcgetattr()/tcsetattr()
 * like upstream Neatvi expects. */
#include "libpure.h"

int tcgetattr(int fd, struct termios *out);
int tcsetattr(int fd, int actions, const struct termios *in);

#endif
