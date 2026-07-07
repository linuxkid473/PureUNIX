/* Shadow for newlib's own <unistd.h> (third_party/newlib) — real header,
 * genuine gap: it declares no ttyname()/ttyname_r() at all for this
 * target. PureUNIX has exactly one console device (drivers/tty.c — no
 * /dev tree, no separate tty device files), so user/newlib_syscalls.c's
 * implementation is a real (if simple) answer: isatty(fd) determines
 * whether to report the one console name or ENOTTY, not a fabricated
 * stub standing in for a missing feature.
 *
 * #include_next finds the next <unistd.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_UNISTD_H
#define PUREUNIX_NEWLIB_COMPAT_UNISTD_H

#include_next <unistd.h>

char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t buflen);

#endif
