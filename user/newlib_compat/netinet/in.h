/* Override for glibc's <netinet/in.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack. Declarations only; nothing in the applets currently
 * enabled in .config is network-related.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_NETINET_IN_H
#define PUREUNIX_NEWLIB_COMPAT_NETINET_IN_H

#include <sys/socket.h>
#include <sys/types.h> /* in_addr_t already comes from here (__uint32_t) —
                         * don't redeclare it with a different underlying
                         * type, or newlib's own sys/types.h and this file
                         * conflict. */

typedef unsigned short in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct in6_addr {
    unsigned char s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    unsigned int    sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int    sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41

/* Real, standard glibc/BSD IP_ and IPV6_ setsockopt() option names — only
 * ever passed to the honest ENOSYS setsockopt()/getsockopt() stubs in
 * user/newlib_syscalls.c on this platform, but the constants themselves
 * are real, standard values. */
#define IP_TOS       1
#define IPV6_TCLASS  67
#define IP_TTL              2
#define IP_MULTICAST_TTL    33
#define IP_MULTICAST_LOOP   34
#define IP_ADD_MEMBERSHIP   35
#define IP_DROP_MEMBERSHIP  36
#define IPV6_UNICAST_HOPS   4
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21

/* Real, standard glibc/BSD multicast-membership request structs — passed
 * to the honest ENOSYS setsockopt() stub above, same reasoning. */
struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

/* Real, standard glibc/BSD presentation-format buffer sizes and IPv4
 * classification macro. */
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

#define IN_MULTICAST(a) (((unsigned int)(a) & 0xf0000000) == 0xe0000000)

/* Real, standard RFC 2553/glibc/BSD IPv6 address globals and classification
 * macros — pure byte-pattern tests/constants, no kernel/network-stack
 * involvement needed even though PureUnix has no real IPv6 support; the
 * variables themselves are defined for real in user/newlib_syscalls.c. */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT      { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 } }

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const unsigned int *)(const void *)(a))[0] == 0 && \
     ((const unsigned int *)(const void *)(a))[1] == 0 && \
     ((const unsigned int *)(const void *)(a))[2] == 0 && \
     ((const unsigned int *)(const void *)(a))[3] == 0)

#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const unsigned int *)(const void *)(a))[0] == 0 && \
     ((const unsigned int *)(const void *)(a))[1] == 0 && \
     ((const unsigned int *)(const void *)(a))[2] == 0 && \
     ((const unsigned char *)(a))[12] == 0 && \
     ((const unsigned char *)(a))[13] == 0 && \
     ((const unsigned char *)(a))[14] == 0 && \
     ((const unsigned char *)(a))[15] == 1)

#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const unsigned int *)(const void *)(a))[0] == 0 && \
     ((const unsigned int *)(const void *)(a))[1] == 0 && \
     ((const unsigned char *)(a))[8] == 0 && \
     ((const unsigned char *)(a))[9] == 0 && \
     ((const unsigned char *)(a))[10] == 0xff && \
     ((const unsigned char *)(a))[11] == 0xff)

#define IN6_IS_ADDR_LINKLOCAL(a) \
    (((const unsigned char *)(a))[0] == 0xfe && \
     (((const unsigned char *)(a))[1] & 0xc0) == 0x80)

#define IN6_IS_ADDR_SITELOCAL(a) \
    (((const unsigned char *)(a))[0] == 0xfe && \
     (((const unsigned char *)(a))[1] & 0xc0) == 0xc0)

#define IN6_IS_ADDR_MULTICAST(a) \
    (((const unsigned char *)(a))[0] == 0xff)

#define IN6_IS_ADDR_MC_NODELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const unsigned char *)(a))[1] & 0x0f) == 0x01)

#define IN6_IS_ADDR_MC_LINKLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const unsigned char *)(a))[1] & 0x0f) == 0x02)

#define IN6_IS_ADDR_MC_SITELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const unsigned char *)(a))[1] & 0x0f) == 0x05)

#define IN6_IS_ADDR_MC_ORGLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const unsigned char *)(a))[1] & 0x0f) == 0x08)

#define IN6_IS_ADDR_MC_GLOBAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const unsigned char *)(a))[1] & 0x0f) == 0x0e)

/* Real glibc/BSD convention: <netinet/in.h> is what actually exposes
 * ntohs/ntohl/htons/htonl to callers (e.g. glib's xdgmimecache.c includes
 * only this header expecting them), even though the real implementations
 * live in arpa/inet.h. Included down here, after struct in_addr is already
 * complete, so arpa/inet.h's inet_ntoa() prototype sees a real definition
 * rather than relying on an implicit incomplete forward declaration. */
#include <arpa/inet.h>

#endif
