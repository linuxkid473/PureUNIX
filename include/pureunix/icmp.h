#ifndef PUREUNIX_ICMP_H
#define PUREUNIX_ICMP_H

#include <pureunix/inet.h>
#include <pureunix/types.h>

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct __attribute__((packed)) icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

/* Registers icmp_input() with the IP layer for IP_PROTO_ICMP. From this
 * point on, incoming echo requests addressed to us are answered
 * automatically (in interrupt context for real traffic -- see
 * net/ip.c/net/eth.c). */
void icmp_init(void);

/* Sends an ICMP echo request ("ping") to `dst` with the given identifier/
 * sequence number and up to `len` bytes of payload, then waits (via
 * pit_sleep(), so this needs arch_enable_interrupts() to already have run)
 * up to timeout_ms for a matching echo reply (same id+seq+source). Returns
 * true on success, filling *rtt_ms (if non-NULL) with an approximate
 * round-trip time. Only one ping can be outstanding at a time -- there is
 * a single global "waiting for this id/seq/source" slot, not a table. */
bool icmp_ping(ip4_addr_t dst, uint16_t id, uint16_t seq, const void *payload, uint16_t len,
               uint32_t timeout_ms, uint32_t *rtt_ms);

/* Diagnostic: pings the loopback address (a fully deterministic, hardware-
 * independent round trip through ip_send()'s loopback shortcut), then the
 * configured default gateway (best-effort -- see docs/networking.md for
 * why a timeout there isn't necessarily a bug), then feeds a synthetic
 * incoming echo request straight into icmp_input(), mirroring net/arp.c's
 * arp_selftest() -- proves "respond to incoming ping" deterministically. */
void icmp_selftest(void);

#endif
