#include <pureunix/arp.h>
#include <pureunix/byteorder.h>
#include <pureunix/eth.h>
#include <pureunix/ip.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define IP_MAX_HANDLERS 4
#define IP_MAX_PAYLOAD  1480 /* 1500 Ethernet MTU - 20-byte IPv4 header */

typedef struct ip_handler_entry {
    uint8_t protocol;
    ip_rx_handler_t handler;
} ip_handler_entry_t;

static ip_handler_entry_t handlers[IP_MAX_HANDLERS];
static int handler_count;

static ip4_addr_t local_addr;
static ip4_addr_t local_netmask;
static ip4_addr_t local_gateway;
static uint16_t next_id = 1;

/* True for destinations ip_send() delivers via the loopback shortcut
 * (127.0.0.0/8, or our own configured address) rather than real Ethernet/
 * ARP -- shared so ip_resolve_route() doesn't try to ARP-resolve a
 * gateway for a destination that was never going to need ARP at all. */
static bool is_loopback_dst(ip4_addr_t dst)
{
    return (dst & 0xFF000000U) == IP4_LOOPBACK_NET || dst == local_addr;
}

/* Routing: a destination inside our own subnet is reached directly;
 * anything else goes via the default gateway -- there's exactly one
 * interface, so this is the entire routing table. Shared by ip_send()
 * (non-blocking) and ip_resolve_route() (blocking) below so the two never
 * disagree about what the next hop is. */
static ip4_addr_t route_next_hop(ip4_addr_t dst)
{
    return ((dst & local_netmask) == (local_addr & local_netmask)) ? dst : local_gateway;
}

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void ip_register_handler(uint8_t protocol, ip_rx_handler_t handler)
{
    if (handler_count < IP_MAX_HANDLERS) {
        handlers[handler_count].protocol = protocol;
        handlers[handler_count].handler = handler;
        ++handler_count;
    }
}

void ip_configure(ip4_addr_t addr, ip4_addr_t netmask, ip4_addr_t gateway)
{
    local_addr = addr;
    local_netmask = netmask;
    local_gateway = gateway;
    arp_set_local_ip(addr);
    printf("ip: configured %u.%u.%u.%u netmask %u.%u.%u.%u gateway %u.%u.%u.%u\n",
           (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
           (netmask >> 24) & 0xFF, (netmask >> 16) & 0xFF, (netmask >> 8) & 0xFF, netmask & 0xFF,
           (gateway >> 24) & 0xFF, (gateway >> 16) & 0xFF, (gateway >> 8) & 0xFF, gateway & 0xFF);
}

ip4_addr_t ip_local_addr(void)
{
    return local_addr;
}

/* Dispatches a fully-parsed, already-validated IPv4 payload to whichever
 * protocol handler (if any) is registered -- shared by the real Ethernet
 * RX path (ip_input()) and the loopback shortcut in ip_send(). */
static void ip_deliver(ip4_addr_t src, ip4_addr_t dst, uint8_t protocol, const uint8_t *payload, uint16_t len)
{
    for (int i = 0; i < handler_count; ++i) {
        if (handlers[i].protocol == protocol) {
            handlers[i].handler(src, dst, payload, len);
            return;
        }
    }
}

/* Registered with the Ethernet layer for ETH_TYPE_IPV4 (see ip_init());
 * runs in interrupt context, same as eth_dispatch()/arp_input() -- see
 * drivers/e1000.c's e1000_irq() comment. */
static void ip_input(const uint8_t src_mac[6], const uint8_t *frame, uint16_t len)
{
    (void)src_mac;
    if (len < IP4_MIN_HEADER_LEN) {
        return;
    }
    const ip4_header_t *hdr = (const ip4_header_t *)frame;
    if ((hdr->ver_ihl >> 4) != 4) {
        return; /* not IPv4 */
    }

    uint8_t ihl_words = hdr->ver_ihl & 0x0FU;
    uint16_t hdr_len = (uint16_t)(ihl_words * 4);
    if (ihl_words < 5 || hdr_len > len) {
        return;
    }
    if (ip_checksum(hdr, hdr_len) != 0) {
        return; /* corrupt header */
    }

    uint16_t flags_frag = ntohs(hdr->flags_frag);
    bool more_fragments = (flags_frag & 0x2000U) != 0;
    uint16_t frag_offset = flags_frag & 0x1FFFU;
    if (more_fragments || frag_offset != 0) {
        /* Fragment rejection: no reassembly support -- silently drop
         * rather than mis-deliver a partial datagram. A stricter stack
         * might send an ICMP "fragmentation needed"/time-exceeded reply
         * here; this phase's scope is just to never act on a fragment. */
        return;
    }

    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len < hdr_len || total_len > len) {
        return;
    }

    ip4_addr_t dst = ntohl(hdr->dst);
    if (dst != local_addr && dst != IP4_BROADCAST) {
        return; /* not addressed to us */
    }

    ip4_addr_t src = ntohl(hdr->src);
    const uint8_t *payload = frame + hdr_len;
    uint16_t payload_len = (uint16_t)(total_len - hdr_len);
    ip_deliver(src, dst, hdr->protocol, payload, payload_len);
}

void ip_init(void)
{
    handler_count = 0;
    eth_register_handler(ETH_TYPE_IPV4, ip_input);
}

bool ip_resolve_route(ip4_addr_t dst, uint8_t mac[6], uint32_t timeout_ms)
{
    if (is_loopback_dst(dst)) {
        /* ip_send() never touches ARP for this destination either --
         * nothing to resolve, and *mac is never actually used for a
         * loopback send. */
        memset(mac, 0, 6);
        return true;
    }
    return arp_resolve(route_next_hop(dst), mac, timeout_ms);
}

int ip_send(ip4_addr_t dst, uint8_t protocol, const void *payload, uint16_t len)
{
    if (len > IP_MAX_PAYLOAD) {
        return -1;
    }

    /* Loopback interface: 127.0.0.0/8, or sending to our own configured
     * address, never touches Ethernet/ARP -- deliver straight back to the
     * registered handler, exactly like a real OS's lo0/"lo" interface.
     * Source is the loopback address itself for a 127.0.0.0/8 destination
     * (so e.g. a "ping 127.0.0.1" looks like a genuine 127.0.0.1<->127.0.0.1
     * conversation to the caller, not "local_addr -> 127.0.0.1" — which
     * would break any caller matching replies against the address it
     * pinged, the way net/icmp.c's icmp_ping() does), or local_addr for the
     * "sent to our own real address" case. */
    if (is_loopback_dst(dst)) {
        ip4_addr_t src = ((dst & 0xFF000000U) == IP4_LOOPBACK_NET) ? dst : local_addr;
        ip_deliver(src, dst, protocol, (const uint8_t *)payload, len);
        return 0;
    }

    ip4_addr_t next_hop = route_next_hop(dst);

    /* Deliberately non-blocking: ip_send() (via ip_input()'s protocol
     * handlers, e.g. icmp_input()'s auto-reply) can run from inside the RX
     * interrupt handler (see drivers/e1000.c's e1000_irq() and
     * net/eth.c's eth_dispatch() comments), where interrupts are globally
     * disabled -- calling the blocking arp_resolve() from there would
     * spin forever in pit_sleep(), since the PIT tick interrupt that
     * pit_sleep() waits on can never fire while nested inside another
     * interrupt handler. A real deadlock, hit and fixed during Phase 3
     * hardening: injecting a real inbound ICMP echo request from outside
     * the guest (bypassing SLIRP via a raw "-netdev socket,udp=..." link)
     * reliably hung the entire kernel solid at exactly this call, with no
     * further boot output, until this fix. Callers in normal (non-
     * interrupt) context that want a reliable send to a not-yet-cached
     * destination should call ip_resolve_route() themselves first (see
     * net/icmp.c's icmp_ping()) to warm the cache before calling ip_send()
     * -- which then hits the cache here and completes immediately. */
    uint8_t dst_mac[6];
    if (!arp_lookup(next_hop, dst_mac)) {
        arp_request(next_hop);
        return -1;
    }

    uint8_t packet[IP4_MIN_HEADER_LEN + IP_MAX_PAYLOAD];
    ip4_header_t *hdr = (ip4_header_t *)packet;
    hdr->ver_ihl = 0x45; /* version 4, IHL 5 (20 bytes, no options) */
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)(IP4_MIN_HEADER_LEN + len));
    hdr->id = htons(next_id++);
    hdr->flags_frag = htons(0x4000U); /* DF set; this stack never fragments anyway */
    hdr->ttl = IP4_DEFAULT_TTL;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src = htonl(local_addr);
    hdr->dst = htonl(dst);
    hdr->checksum = htons(ip_checksum(hdr, IP4_MIN_HEADER_LEN));
    memcpy(packet + IP4_MIN_HEADER_LEN, payload, len);

    return eth_send(dst_mac, ETH_TYPE_IPV4, packet, (uint16_t)(IP4_MIN_HEADER_LEN + len));
}
