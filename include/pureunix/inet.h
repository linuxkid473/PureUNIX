#ifndef PUREUNIX_INET_H
#define PUREUNIX_INET_H

#include <pureunix/types.h>

/* An IPv4 address in host byte order -- i.e. IP4_ADDR(192,168,1,1) equals
 * the "obvious" 0xC0A80101, with the first octet in the most significant
 * byte. There is no IP stack consumer outside this kernel yet, so this
 * convention (rather than raw wire-order bytes, used by net/arp.c before
 * this type existed) is free to be whatever's most convenient for
 * arithmetic -- routing decisions (net/ip.c's subnet check) need a real
 * bitwise AND against a netmask, which wire-order byte arrays can't do
 * directly. Use htonl()/ntohl() (include/pureunix/byteorder.h) when
 * reading/writing an IPv4 header's 32-bit address fields, exactly like
 * real network code does for struct in_addr. */
typedef uint32_t ip4_addr_t;

#define IP4_ADDR(a, b, c, d) \
    (((ip4_addr_t)(uint8_t)(a) << 24) | ((ip4_addr_t)(uint8_t)(b) << 16) | \
     ((ip4_addr_t)(uint8_t)(c) << 8)  |  (ip4_addr_t)(uint8_t)(d))

#define IP4_LOOPBACK_NET IP4_ADDR(127, 0, 0, 0)
#define IP4_BROADCAST    0xFFFFFFFFU

static inline void ip4_to_bytes(ip4_addr_t ip, uint8_t out[4])
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)(ip);
}

static inline ip4_addr_t ip4_from_bytes(const uint8_t in[4])
{
    return ((ip4_addr_t)in[0] << 24) | ((ip4_addr_t)in[1] << 16) |
           ((ip4_addr_t)in[2] << 8)  |  (ip4_addr_t)in[3];
}

#endif
