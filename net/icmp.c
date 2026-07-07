#include <pureunix/arch.h>
#include <pureunix/byteorder.h>
#include <pureunix/e1000.h>
#include <pureunix/icmp.h>
#include <pureunix/ip.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define ICMP_MAX_PAYLOAD 1024

/* Only one ping outstanding at a time: a single "what am I waiting for"
 * slot rather than a table of concurrent requests. reply_seen is set from
 * interrupt context (icmp_input(), reached via the RX interrupt) and
 * polled from normal context (icmp_ping()'s wait loop) -- safe without a
 * lock since this kernel is single-core and non-preemptive within an
 * interrupt (see drivers/e1000.c's e1000_irq() comment), so the two never
 * run concurrently. */
static volatile bool reply_pending;
static volatile bool reply_seen;
static uint16_t wait_id;
static uint16_t wait_seq;
static ip4_addr_t wait_src;

/* Registered with the IP layer for IP_PROTO_ICMP (see icmp_init()); runs
 * in interrupt context for real traffic, same as ip_input()/arp_input()
 * -- see drivers/e1000.c's e1000_irq() comment. */
static void icmp_input(ip4_addr_t src, ip4_addr_t dst, const uint8_t *payload, uint16_t len)
{
    (void)dst;
    if (len < sizeof(icmp_header_t)) {
        return;
    }
    if (ip_checksum(payload, len) != 0) {
        return; /* corrupt packet */
    }
    const icmp_header_t *hdr = (const icmp_header_t *)payload;

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
        printf("icmp: echo request from %u.%u.%u.%u id=%u seq=%u\n",
               (src >> 24) & 0xFF, (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
               ntohs(hdr->id), ntohs(hdr->seq));

        uint8_t reply[sizeof(icmp_header_t) + ICMP_MAX_PAYLOAD];
        uint16_t reply_len = (len > sizeof(reply)) ? (uint16_t)sizeof(reply) : len;
        memcpy(reply, payload, reply_len);
        icmp_header_t *rhdr = (icmp_header_t *)reply;
        rhdr->type = ICMP_TYPE_ECHO_REPLY;
        rhdr->code = 0;
        rhdr->checksum = 0;
        rhdr->checksum = htons(ip_checksum(reply, reply_len));

        int rc = ip_send(src, IP_PROTO_ICMP, reply, reply_len);
        printf("icmp: echo reply %s\n", rc == 0 ? "sent" : "failed (no route/ARP timeout)");
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REPLY && reply_pending &&
        ntohs(hdr->id) == wait_id && ntohs(hdr->seq) == wait_seq && src == wait_src) {
        reply_seen = true;
    }
}

void icmp_init(void)
{
    ip_register_handler(IP_PROTO_ICMP, icmp_input);
}

bool icmp_ping(ip4_addr_t dst, uint16_t id, uint16_t seq, const void *payload, uint16_t len,
               uint32_t timeout_ms, uint32_t *rtt_ms)
{
    if (len > ICMP_MAX_PAYLOAD) {
        return false;
    }

    uint8_t packet[sizeof(icmp_header_t) + ICMP_MAX_PAYLOAD];
    icmp_header_t *hdr = (icmp_header_t *)packet;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->seq = htons(seq);
    if (len) {
        memcpy(packet + sizeof(icmp_header_t), payload, len);
    }
    uint16_t total = (uint16_t)(sizeof(icmp_header_t) + len);
    hdr->checksum = htons(ip_checksum(packet, total));

    /* ip_send() itself never blocks (it can run from RX interrupt context
     * -- see its comment in net/ip.c), so it fails outright on a cold ARP
     * cache. icmp_ping() always runs in normal (task) context, so it's
     * safe here to pre-resolve (blocking, up to a flat 1s) before
     * attempting the actual send, giving ip_send() a warm cache to hit. */
    uint8_t next_hop_mac[6];
    if (!ip_resolve_route(dst, next_hop_mac, 1000)) {
        return false;
    }

    wait_id = id;
    wait_seq = seq;
    wait_src = dst;
    reply_seen = false;
    reply_pending = true;

    uint64_t start_ticks = pit_ticks();
    if (ip_send(dst, IP_PROTO_ICMP, packet, total) != 0) {
        reply_pending = false;
        return false;
    }

    for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
        if (reply_seen) {
            reply_pending = false;
            if (rtt_ms) {
                /* kernel/main.c calls pit_init(100) -- 100 ticks/sec, so
                 * each tick is 10ms. */
                *rtt_ms = (uint32_t)(pit_ticks() - start_ticks) * 10U;
            }
            return true;
        }
        pit_sleep(10);
    }
    reply_pending = false;
    return false;
}

void icmp_selftest(void)
{
    if (!e1000_present()) {
        return;
    }
    static const char msg[] = "PureUNIX ping";
    uint32_t rtt;

    /* 1. Loopback: fully deterministic, never touches Ethernet/ARP/hardware
     * -- exercises the complete encode/checksum/route/decode/checksum
     * round trip. */
    if (icmp_ping(IP4_ADDR(127, 0, 0, 1), 1, 1, msg, (uint16_t)sizeof(msg) - 1, 500, &rtt)) {
        printf("icmp: self-test loopback ping OK (%u ms)\n", rtt);
    } else {
        printf("icmp: self-test loopback ping FAILED\n");
    }

    /* 2. Real network: best-effort, see docs/networking.md. */
    if (icmp_ping(IP4_ADDR(10, 0, 2, 2), 1, 2, msg, (uint16_t)sizeof(msg) - 1, 1000, &rtt)) {
        printf("icmp: self-test ping to gateway OK (%u ms)\n", rtt);
    } else {
        printf("icmp: self-test got no ping reply from gateway (depends on network backend reachability)\n");
    }

    /* 3. Synthetic incoming echo request -- proves "respond to incoming
     * ping" deterministically, independent of network reachability;
     * mirrors net/arp.c's arp_selftest(). */
    icmp_header_t req;
    req.type = ICMP_TYPE_ECHO_REQUEST;
    req.code = 0;
    req.checksum = 0;
    req.id = htons(0xBEEF);
    req.seq = htons(1);
    req.checksum = htons(ip_checksum(&req, sizeof(req)));
    icmp_input(IP4_ADDR(192, 0, 2, 1), ip_local_addr(), (const uint8_t *)&req, sizeof(req));
}
