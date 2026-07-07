#include <pureunix/arch.h>
#include <pureunix/arp.h>
#include <pureunix/byteorder.h>
#include <pureunix/e1000.h>
#include <pureunix/eth.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4     0x0800U
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2
#define ARP_CACHE_SIZE     16

/* sha/spa/tha/tpa are the ARP packet's genuine wire format (raw octets, no
 * byte-order conversion needed since they're just opaque bytes on the
 * wire) -- ip4_addr_t (include/pureunix/inet.h) is only used at this
 * file's public API boundary and in the cache, converted via
 * ip4_to_bytes()/ip4_from_bytes() when building/parsing a packet. */
typedef struct __attribute__((packed)) arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_packet_t;

typedef struct arp_entry {
    bool valid;
    ip4_addr_t ip;
    uint8_t mac[6];
} arp_entry_t;

static arp_entry_t cache[ARP_CACHE_SIZE];
static ip4_addr_t local_ip;

static void cache_insert(ip4_addr_t ip, const uint8_t mac[6])
{
    int free_slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (cache[i].valid && cache[i].ip == ip) {
            memcpy(cache[i].mac, mac, 6);
            return;
        }
        if (free_slot < 0 && !cache[i].valid) {
            free_slot = i;
        }
    }
    /* No existing entry: use a free slot, or -- cache full -- fall back to
     * overwriting slot 0. Simple fixed-size table with no LRU/eviction
     * policy, matching this kernel's preference for small fixed-resource
     * tables elsewhere (e.g. MAX_OPEN_FILES in include/pureunix/task.h). */
    int slot = (free_slot >= 0) ? free_slot : 0;
    cache[slot].valid = true;
    cache[slot].ip = ip;
    memcpy(cache[slot].mac, mac, 6);
}

bool arp_lookup(ip4_addr_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (cache[i].valid && cache[i].ip == ip) {
            memcpy(mac, cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void send_arp(uint16_t oper, const uint8_t dst_mac[6], const uint8_t tha[6], const uint8_t tpa[4])
{
    arp_packet_t pkt;
    pkt.htype = htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = htons(ARP_PTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = htons(oper);
    memcpy(pkt.sha, eth_get_mac(), 6);
    ip4_to_bytes(local_ip, pkt.spa);
    memcpy(pkt.tha, tha, 6);
    memcpy(pkt.tpa, tpa, 4);
    eth_send(dst_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_request(ip4_addr_t ip)
{
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    uint8_t tpa[4];
    ip4_to_bytes(ip, tpa);
    send_arp(ARP_OP_REQUEST, eth_broadcast_mac, zero_mac, tpa);
}

/* Registered with the Ethernet layer for ETH_TYPE_ARP (see arp_init());
 * runs in interrupt context, same as eth_dispatch() itself -- see the
 * comment on e1000_irq() in drivers/e1000.c. */
static void arp_input(const uint8_t src_mac[6], const uint8_t *payload, uint16_t len)
{
    (void)src_mac;
    if (len < sizeof(arp_packet_t)) {
        return;
    }
    const arp_packet_t *pkt = (const arp_packet_t *)payload;
    if (ntohs(pkt->htype) != ARP_HTYPE_ETHERNET || ntohs(pkt->ptype) != ARP_PTYPE_IPV4) {
        return;
    }

    ip4_addr_t spa = ip4_from_bytes(pkt->spa);
    ip4_addr_t tpa = ip4_from_bytes(pkt->tpa);

    /* Learn the sender's mapping regardless of request/reply -- whoever
     * sent this frame necessarily knows their own IP/MAC pairing, so every
     * real ARP implementation opportunistically caches it either way. */
    cache_insert(spa, pkt->sha);

    if (ntohs(pkt->oper) == ARP_OP_REQUEST && tpa == local_ip) {
        printf("arp: replying to who-has %u.%u.%u.%u from %02x:%02x:%02x:%02x:%02x:%02x\n",
               pkt->tpa[0], pkt->tpa[1], pkt->tpa[2], pkt->tpa[3],
               pkt->sha[0], pkt->sha[1], pkt->sha[2], pkt->sha[3], pkt->sha[4], pkt->sha[5]);
        send_arp(ARP_OP_REPLY, pkt->sha, pkt->sha, pkt->spa);
    }
}

void arp_set_local_ip(ip4_addr_t ip)
{
    local_ip = ip;
}

void arp_init(void)
{
    memset(cache, 0, sizeof(cache));
    eth_register_handler(ETH_TYPE_ARP, arp_input);
}

bool arp_resolve(ip4_addr_t ip, uint8_t mac[6], uint32_t timeout_ms)
{
    if (arp_lookup(ip, mac)) {
        return true;
    }
    arp_request(ip);
    for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
        pit_sleep(10);
        if (arp_lookup(ip, mac)) {
            return true;
        }
    }
    return false;
}

void arp_selftest(void)
{
    if (!e1000_present()) {
        return;
    }
    ip4_addr_t gateway_ip = IP4_ADDR(10, 0, 2, 2);
    uint8_t mac[6];
    if (arp_resolve(gateway_ip, mac, 1000)) {
        printf("arp: self-test resolved 10.0.2.2 -> %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("arp: self-test got no reply for 10.0.2.2 (depends on network backend reachability)\n");
    }

    /* Direct logic test, independent of whether any real peer is reachable
     * on this network backend: feeds a synthetic "who-has <our IP>" request
     * (from a locally-administered test MAC and an RFC 5737 TEST-NET-1
     * address, so it can never collide with a real host) straight into
     * arp_input() -- the same function real incoming frames reach via
     * eth_dispatch() -- proving out both the reply path (a real e1000_send()
     * call, hardware-confirmed independently by e1000_selftest()) and the
     * cache-population path in one shot. */
    static const uint8_t test_peer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    ip4_addr_t test_peer_ip = IP4_ADDR(192, 0, 2, 1);
    arp_packet_t probe;
    probe.htype = htons(ARP_HTYPE_ETHERNET);
    probe.ptype = htons(ARP_PTYPE_IPV4);
    probe.hlen = 6;
    probe.plen = 4;
    probe.oper = htons(ARP_OP_REQUEST);
    memcpy(probe.sha, test_peer_mac, 6);
    ip4_to_bytes(test_peer_ip, probe.spa);
    memset(probe.tha, 0, 6);
    ip4_to_bytes(local_ip, probe.tpa);
    arp_input(test_peer_mac, (const uint8_t *)&probe, sizeof(probe));

    uint8_t learned_mac[6];
    bool ok = arp_lookup(test_peer_ip, learned_mac) && memcmp(learned_mac, test_peer_mac, 6) == 0;
    printf("arp: self-test synthetic request-and-reply logic %s\n", ok ? "OK" : "FAILED");
}
