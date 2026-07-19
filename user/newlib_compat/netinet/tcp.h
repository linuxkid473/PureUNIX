/* Override for glibc's <netinet/tcp.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack, so these are real, standard option-name constants
 * (matching glibc/BSD values) for use with the honest ENOSYS socket()
 * family in user/newlib_syscalls.c; nothing on this platform can actually
 * set them on a live socket.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_NETINET_TCP_H
#define PUREUNIX_NEWLIB_COMPAT_NETINET_TCP_H

#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_CORK      3
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6

#endif
