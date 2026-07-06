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

#endif
