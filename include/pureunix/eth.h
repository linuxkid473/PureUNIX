#ifndef PUREUNIX_ETH_H
#define PUREUNIX_ETH_H

#include <pureunix/types.h>

#define ETH_ALEN        6
#define ETH_TYPE_ARP    0x0806U
#define ETH_TYPE_IPV4   0x0800U

/* Wire-format Ethernet II header -- ethertype is big-endian ("network byte
 * order"); use the ethertype passed to eth_register_handler()/eth_send()
 * in host order, the layer converts. */
typedef struct __attribute__((packed)) eth_header {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} eth_header_t;

extern const uint8_t eth_broadcast_mac[ETH_ALEN];

/* Called once per received frame matching a registered ethertype, with
 * `payload`/`len` pointing just past the 14-byte Ethernet header (i.e. the
 * ARP packet, IP datagram, etc. -- never the header itself). Runs in
 * interrupt context (see drivers/e1000.c's e1000_irq() -> eth_dispatch()
 * chain), so handlers must not block. */
typedef void (*eth_rx_handler_t)(const uint8_t src_mac[ETH_ALEN], const uint8_t *payload, uint16_t len);

/* Registers eth_dispatch() as the e1000 driver's RX callback -- from this
 * point on, every arriving frame is parsed and dispatched automatically
 * whenever the NIC's RX interrupt fires. Must be called after e1000_init(). */
void eth_init(void);

/* Adds a handler for `ethertype` (host byte order, e.g. ETH_TYPE_ARP).
 * Up to ETH_MAX_HANDLERS handlers total; extra registrations are ignored. */
void eth_register_handler(uint16_t ethertype, eth_rx_handler_t handler);

/* Copies this host's own MAC address (from the e1000 driver) into a
 * static buffer and returns a pointer to it -- valid until the next call. */
const uint8_t *eth_get_mac(void);

/* Builds an Ethernet II frame (dst, our own MAC as src, ethertype in host
 * order converted to wire order) around `payload`/`len`, padding up to the
 * 60-byte minimum frame size if needed, and transmits it. Returns 0 on
 * success, -1 if len is too large for one frame or the underlying send
 * fails. */
int eth_send(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len);

#endif
