/* Override for glibc's <sys/socket.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack at all. Declarations only, so code that doesn't actually
 * call these (true of every applet currently enabled in .config) still
 * compiles; anything that does call one will fail to link, same as any
 * other syscall PureUNIX doesn't have yet.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_SOCKET_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_BROADCAST 6
#define SO_ERROR 4
#define SO_TYPE  3
#define SOMAXCONN 128

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
int socketpair(int domain, int type, int protocol, int sv[2]);
long recv(int fd, void *buf, size_t len, int flags);
long send(int fd, const void *buf, size_t len, int flags);
long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

/* Real, standard glibc/BSD ancillary-data (msghdr/cmsghdr/CMSG_*) API —
 * struct layout and CMSG_* macro formulas match the well-known glibc
 * implementation exactly (alignment to sizeof(size_t)). sendmsg()/
 * recvmsg() themselves are honest ENOSYS stubs like the rest of this
 * file's socket family, since no real AF_UNIX fd-passing exists on this
 * platform — but nothing here is fabricated: any caller that builds a
 * struct msghdr before finding out the call fails gets real, correctly
 * laid-out structures to build it with. */
#include <sys/uio.h>

struct msghdr {
    void          *msg_name;
    socklen_t      msg_namelen;
    struct iovec  *msg_iov;
    size_t         msg_iovlen;
    void          *msg_control;
    size_t         msg_controllen;
    int            msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_TRUNC     0x20
#define MSG_CTRUNC    0x08
#define MSG_WAITALL   0x100
#define MSG_EOR       0x80
#define MSG_NOSIGNAL  0x4000
#define MSG_WAITFORONE 0x10000

#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2
#define SCM_CREDS       SCM_CREDENTIALS

#define __PU_CMSG_ALIGN(len) \
    (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))

#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))

#define CMSG_FIRSTHDR(mhdr) \
    ((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
     ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)

#define CMSG_NXTHDR(mhdr, cmsg) \
    (((cmsg) == (struct cmsghdr *)0) ? CMSG_FIRSTHDR(mhdr) : \
     (((unsigned char *)(cmsg) + __PU_CMSG_ALIGN((cmsg)->cmsg_len) + sizeof(struct cmsghdr) \
       > (unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen) \
      ? (struct cmsghdr *)0 \
      : (struct cmsghdr *)((unsigned char *)(cmsg) + __PU_CMSG_ALIGN((cmsg)->cmsg_len))))

#define CMSG_LEN(len)   (__PU_CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_SPACE(len) (__PU_CMSG_ALIGN(sizeof(struct cmsghdr)) + __PU_CMSG_ALIGN(len))

long sendmsg(int fd, const struct msghdr *msg, int flags);
long recvmsg(int fd, struct msghdr *msg, int flags);

#ifdef __cplusplus
}
#endif
#endif
