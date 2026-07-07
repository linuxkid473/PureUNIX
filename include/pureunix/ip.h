#ifndef PUREUNIX_IP_H
#define PUREUNIX_IP_H

#include <pureunix/inet.h>
#include <pureunix/types.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IP4_MIN_HEADER_LEN 20
#define IP4_DEFAULT_TTL    64

/* Wire-format IPv4 header (no options -- ver_ihl is always 0x45 on send;
 * ip_input() below tolerates a larger IHL on receive by skipping past the
 * options rather than parsing them). src/dst are in network byte order;
 * use ntohl()/htonl() (include/pureunix/byteorder.h) to convert to/from an
 * ip4_addr_t. */
typedef struct __attribute__((packed)) ip4_header {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ip4_header_t;

typedef void (*ip_rx_handler_t)(ip4_addr_t src, ip4_addr_t dst, const uint8_t *payload, uint16_t len);

/* Generic Internet checksum (RFC 1071) over `len` bytes at `data`. The
 * returned value, once byte-swapped into wire order via htons(), is what
 * belongs in a checksum field; equivalently, running this over data that
 * already contains a correctly-computed checksum field (the normal way to
 * verify one on receive) returns exactly 0. Shared by net/ip.c and
 * net/icmp.c, since both protocols use the same algorithm. */
uint16_t ip_checksum(const void *data, size_t len);

/* Registers a handler for `protocol` (IP_PROTO_*) -- called for every
 * datagram addressed to us (or loopback) carrying that protocol number,
 * with `payload`/`len` pointing just past the IP header. Only one handler
 * per protocol; a later registration replaces an earlier one. Runs in
 * interrupt context for real (non-loopback) traffic -- see net/eth.c's
 * eth_dispatch()/drivers/e1000.c's e1000_irq() comment. */
void ip_register_handler(uint8_t protocol, ip_rx_handler_t handler);

/* Simple network configuration: sets this host's own address/netmask, and
 * (optionally, pass 0 if none) a default gateway used for destinations
 * outside the local subnet -- see ip_send()'s routing logic. There is
 * exactly one interface, so this is the whole configuration surface.
 * Propagates addr to the ARP layer (arp_set_local_ip()) too. */
void ip_configure(ip4_addr_t addr, ip4_addr_t netmask, ip4_addr_t gateway);
ip4_addr_t ip_local_addr(void);

/* Registers ip_input() with the Ethernet layer for ETH_TYPE_IPV4. */
void ip_init(void);

/* Resolves the MAC address of whatever ip_send() would treat as `dst`'s
 * next hop (dst itself if it's in our subnet, otherwise the configured
 * gateway), blocking (via arp_resolve()'s pit_sleep()-based wait) for up
 * to timeout_ms if it isn't already cached. Callers MUST be in normal
 * (non-interrupt) context -- see ip_send()'s own comment for why it
 * cannot do this blocking wait itself. Call this before ip_send() to
 * reliably deliver to a destination that might not be ARP-cached yet
 * (see net/icmp.c's icmp_ping()); once cached, ip_send() completes
 * immediately without blocking. */
bool ip_resolve_route(ip4_addr_t dst, uint8_t mac[6], uint32_t timeout_ms);

/* Sends `len` bytes of `payload` as a single (unfragmented) IPv4 datagram
 * from our own configured address to `dst`, carrying protocol number
 * `protocol`. Handles routing (destinations in our own subnet are ARP-
 * resolved directly; anything else goes via the configured gateway)
 * automatically, but next-hop MAC resolution is cache-only and NEVER
 * blocks -- if the next hop isn't already ARP-cached, this fires off an
 * ARP request and returns -1 immediately (see the comment in net/ip.c for
 * why: this function can run from RX interrupt context, e.g. via
 * icmp_input()'s auto-reply path, where blocking is a real deadlock, not
 * just a stall). Callers that need reliable delivery to a possibly-
 * uncached destination should call ip_resolve_route() first. Destinations
 * in 127.0.0.0/8, or our own configured address, are delivered straight
 * back to the registered protocol handler without ever touching
 * Ethernet/ARP -- PureUNIX's loopback interface, unaffected by any of the
 * above. Returns 0 on success, -1 on failure (oversized payload, ARP
 * cache miss, or a lower-layer send failure). */
int ip_send(ip4_addr_t dst, uint8_t protocol, const void *payload, uint16_t len);

#endif
