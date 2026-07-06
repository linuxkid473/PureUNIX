/* Override for glibc's <net/if.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack. Declarations only; nothing in the applets currently
 * enabled in .config is network-related.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_NET_IF_H
#define PUREUNIX_NEWLIB_COMPAT_NET_IF_H

#include <sys/socket.h>

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        int             ifr_ifindex;
        int             ifr_flags;
        int             ifr_mtu;
    } ifr_ifru;
};

unsigned int if_nametoindex(const char *ifname);
char *if_indextoname(unsigned int ifindex, char *ifname);

#endif
