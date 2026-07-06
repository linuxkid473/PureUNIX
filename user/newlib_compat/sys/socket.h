/* Override for glibc's <sys/socket.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack at all. Declarations only, so code that doesn't actually
 * call these (true of every applet currently enabled in .config) still
 * compiles; anything that does call one will fail to link, same as any
 * other syscall PureUNIX doesn't have yet.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_SOCKET_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_SOCKET_H

#include <stddef.h>

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_BROADCAST 6

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        ss_padding[128 - sizeof(sa_family_t)];
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
long recv(int fd, void *buf, size_t len, int flags);
long send(int fd, const void *buf, size_t len, int flags);
long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

#endif
