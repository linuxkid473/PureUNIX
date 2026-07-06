/* Override for glibc's <poll.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * poll()/select() syscall yet. include/libbb.h includes this
 * unconditionally; kept to just enough to satisfy declarations
 * (struct pollfd, POLLIN/POLLOUT/...) — poll() itself isn't implemented,
 * so linking a program that actually calls it will fail, same as any
 * other syscall PureUNIX doesn't have yet.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_POLL_H
#define PUREUNIX_NEWLIB_COMPAT_POLL_H

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
