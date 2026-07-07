#ifndef PUREUNIX_BYTEORDER_H
#define PUREUNIX_BYTEORDER_H

#include <pureunix/types.h>

/* i686 is always little-endian, so "host to network" is always a byte
 * swap and "network to host" is the identical operation -- no #ifdef
 * needed for a big-endian host, since this kernel only ever targets i686
 * (see Makefile's -march=i686). */

static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |
           ((v & 0x00FF0000U) >> 8)  | ((v & 0xFF000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}

#endif
