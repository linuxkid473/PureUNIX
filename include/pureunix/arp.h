#ifndef PUREUNIX_ARP_H
#define PUREUNIX_ARP_H

#include <pureunix/inet.h>
#include <pureunix/types.h>

/* Sets this host's own IPv4 address -- used as the sender address in
 * outgoing ARP requests/replies and to decide whether an incoming
 * "who-has" request is addressed to us. Normally called indirectly via
 * net/ip.c's ip_configure(), which is the actual "simple network
 * configuration" entry point (see include/pureunix/ip.h); exposed here too
 * for callers that want ARP without the IP layer. */
void arp_set_local_ip(ip4_addr_t ip);

/* Registers the ARP ethertype handler with the Ethernet layer -- must be
 * called after eth_init(). From this point on, incoming ARP requests for
 * our own IP are answered automatically (in interrupt context, see
 * net/eth.c), and every ARP packet seen (request or reply) opportunistically
 * updates the cache with its sender's IP/MAC pairing. */
void arp_init(void);

/* Looks up ip in the ARP cache without sending anything. Returns true
 * and fills mac[6] on a hit. */
bool arp_lookup(ip4_addr_t ip, uint8_t mac[6]);

/* Broadcasts an ARP "who-has ip" request and returns immediately, without
 * waiting for a reply -- arp_resolve() below calls this internally;
 * exposed separately for callers that just want to prime the cache
 * asynchronously. */
void arp_request(ip4_addr_t ip);

/* Resolves ip to a MAC address: returns immediately on a cache hit,
 * otherwise sends a request and polls the cache (via pit_sleep(), so this
 * needs arch_enable_interrupts() to already have run) for up to
 * timeout_ms for the reply to arrive. Returns true and fills mac[6] on
 * success, false on timeout. */
bool arp_resolve(ip4_addr_t ip, uint8_t mac[6], uint32_t timeout_ms);

/* Diagnostic: resolves the local network's gateway (10.0.2.2 under QEMU's
 * SLIRP backend) and prints the result -- exercises arp_request()/
 * arp_resolve() and the interrupt-driven arp_input() reply path together.
 * Needs arch_enable_interrupts() to already have run (arp_resolve() waits
 * via pit_sleep()). No-op if !e1000_present(). */
void arp_selftest(void);

#endif
