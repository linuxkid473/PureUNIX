# API Reference: Networking

---

## Ethernet

**Header**: `<pureunix/eth.h>`

```c
#define ETH_ALEN      6
#define ETH_TYPE_ARP  0x0806U
#define ETH_TYPE_IPV4 0x0800U

typedef struct __attribute__((packed)) eth_header {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;   // wire (big-endian) order
} eth_header_t;

extern const uint8_t eth_broadcast_mac[ETH_ALEN];

typedef void (*eth_rx_handler_t)(const uint8_t src_mac[ETH_ALEN], const uint8_t *payload, uint16_t len);

void eth_init(void);
void eth_register_handler(uint16_t ethertype, eth_rx_handler_t handler);  // ethertype in host order
const uint8_t *eth_get_mac(void);
int eth_send(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len);
```

`eth_init()` registers `net/eth.c`'s internal dispatcher as the e1000 driver's RX callback (`e1000_set_rx_handler()`) — from then on, every arriving frame is parsed and handed to whichever handler (if any) is registered for its ethertype, automatically, from inside the NIC's RX interrupt. Handlers therefore run in interrupt context and must not block.

`eth_send()` builds a 14-byte Ethernet II header (destination, our own MAC as source, ethertype converted to wire order) around `payload`, zero-pads up to the 60-byte minimum frame size if needed, and transmits it via `e1000_send()`.

---

## ARP

**Header**: `<pureunix/arp.h>`

```c
void arp_set_local_ip(ip4_addr_t ip);
void arp_init(void);

bool arp_lookup(ip4_addr_t ip, uint8_t mac[6]);
void arp_request(ip4_addr_t ip);
bool arp_resolve(ip4_addr_t ip, uint8_t mac[6], uint32_t timeout_ms);

void arp_selftest(void);
```

`arp_init()` registers `arp_input()` with the Ethernet layer for `ETH_TYPE_ARP`. From then on:
- Every ARP packet seen (request *or* reply) opportunistically updates the cache with its sender's IP/MAC pairing.
- A request whose target IP matches `arp_set_local_ip()`'s address is answered automatically, in interrupt context.

`arp_resolve()` returns immediately on a cache hit; otherwise it calls `arp_request()` (a broadcast "who-has") and polls the cache via `pit_sleep(10)` in a loop until `timeout_ms` elapses — this needs `arch_enable_interrupts()` to already have run, both because `pit_sleep()` depends on the PIT tick interrupt and because the reply that populates the cache only arrives via the e1000 RX interrupt.

Normally called indirectly via `ip_configure()` below, which is the real "simple network configuration" entry point.

---

## IPv4 addressing

**Header**: `<pureunix/inet.h>`

```c
typedef uint32_t ip4_addr_t;   // host byte order, e.g. IP4_ADDR(192,168,1,1) == 0xC0A80101

#define IP4_ADDR(a, b, c, d) /* ... */
#define IP4_LOOPBACK_NET IP4_ADDR(127, 0, 0, 0)
#define IP4_BROADCAST    0xFFFFFFFFU

void ip4_to_bytes(ip4_addr_t ip, uint8_t out[4]);
ip4_addr_t ip4_from_bytes(const uint8_t in[4]);
```

Shared by `arp.h`, `ip.h`, and `icmp.h`. See `docs/networking.md`'s "IPv4 address representation" note for why this is a host-order `uint32_t` rather than a wire-order byte array.

---

## Byte order

**Header**: `<pureunix/byteorder.h>`

```c
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);
```

i686 is always little-endian, so these are unconditionally byte-swaps (`ntohs`/`ntohl` are identical to their `hton*` counterparts) — no big-endian-host case to handle, since this kernel only ever targets i686.

---

## IPv4

**Header**: `<pureunix/ip.h>`

```c
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IP4_MIN_HEADER_LEN 20
#define IP4_DEFAULT_TTL    64

typedef struct __attribute__((packed)) ip4_header {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;   // network byte order -- use ntohl()/htonl()
    uint32_t dst;   // network byte order -- use ntohl()/htonl()
} ip4_header_t;

typedef void (*ip_rx_handler_t)(ip4_addr_t src, ip4_addr_t dst, const uint8_t *payload, uint16_t len);

uint16_t ip_checksum(const void *data, size_t len);
void ip_register_handler(uint8_t protocol, ip_rx_handler_t handler);
void ip_configure(ip4_addr_t addr, ip4_addr_t netmask, ip4_addr_t gateway);
ip4_addr_t ip_local_addr(void);
void ip_init(void);
bool ip_resolve_route(ip4_addr_t dst, uint8_t mac[6], uint32_t timeout_ms);
int ip_send(ip4_addr_t dst, uint8_t protocol, const void *payload, uint16_t len);
```

`ip_checksum()` implements RFC 1071: the value it returns, once passed through `htons()`, belongs in a checksum field; running it over data that already contains a valid checksum returns exactly 0 (the standard verify-without-zeroing-the-field trick).

`ip_send()` routes automatically (local subnet vs. default gateway, from `ip_configure()`'s netmask/gateway) and short-circuits `127.0.0.0/8` destinations (and our own configured address) straight back to the registered handler without ever touching Ethernet/ARP — PureUNIX's loopback interface. Next-hop MAC resolution is **cache-only and never blocks** — a cache miss just fires an ARP request and returns -1 immediately, since this function can run from RX interrupt context (e.g. via `icmp_input()`'s auto-reply path), where the alternative (blocking) is a real deadlock, not just a stall — see `docs/networking.md`'s "`ip_send()` never blocks" section for the bug this was. Callers in normal (non-interrupt) context needing reliable delivery to a possibly-uncached destination should call `ip_resolve_route()` (blocking, same routing/next-hop logic, loopback-aware) first to warm the cache.

---

## ICMP

**Header**: `<pureunix/icmp.h>`

```c
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct __attribute__((packed)) icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

void icmp_init(void);
bool icmp_ping(ip4_addr_t dst, uint16_t id, uint16_t seq, const void *payload, uint16_t len,
               uint32_t timeout_ms, uint32_t *rtt_ms);
void icmp_selftest(void);
```

`icmp_init()` registers an automatic echo-request responder with the IP layer. `icmp_ping()` sends an echo request and polls (via `pit_sleep()`, so needs `arch_enable_interrupts()` to already have run) for a matching reply up to `timeout_ms`; only one ping can be outstanding at a time.

See `docs/networking.md` for the full design writeup (including a real loopback-source-address bug this phase's self-test caught and fixed) and `docs/drivers.md`'s "Intel e1000 NIC Driver" section for how frames physically arrive.
