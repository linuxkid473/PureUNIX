/* Override for glibc's <arpa/inet.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack. htons/ntohs/htonl/ntohl are real (i686 is little-endian,
 * so byte-swapping is well-defined regardless of whether anything ever
 * actually calls them); the address-string functions are declarations
 * only — nothing in the applets currently enabled in .config is
 * network-related.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_ARPA_INET_H
#define PUREUNIX_NEWLIB_COMPAT_ARPA_INET_H

#include <netinet/in.h>

static __inline__ unsigned short htons(unsigned short x)
{
    return (unsigned short)(((x & 0xff) << 8) | ((x >> 8) & 0xff));
}

static __inline__ unsigned short ntohs(unsigned short x)
{
    return htons(x);
}

static __inline__ unsigned int htonl(unsigned int x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
}

static __inline__ unsigned int ntohl(unsigned int x)
{
    return htonl(x);
}

char *inet_ntoa(struct in_addr in);
int inet_aton(const char *cp, struct in_addr *inp);
in_addr_t inet_addr(const char *cp);
const char *inet_ntop(int af, const void *src, char *dst, unsigned int size);
int inet_pton(int af, const char *src, void *dst);

#endif
