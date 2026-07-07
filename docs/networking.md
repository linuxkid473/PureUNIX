# Networking

The networking stack, built in phases on top of the Intel e1000 driver (`docs/drivers.md`): an Ethernet frame layer and ARP (Phase 2), then IPv4 and ICMP (Phase 3). No UDP/TCP yet — that's a later phase.

---

## Ethernet Frame Layer

**Source**: `net/eth.c`
**Header**: `include/pureunix/eth.h`

### Responsibilities

- Parses/builds the 14-byte Ethernet II header (`eth_header_t`: 6-byte destination MAC, 6-byte source MAC, 2-byte big-endian ethertype).
- Dispatches every received frame to whichever handler (if any) is registered for its ethertype, via a small fixed-size table (`ETH_MAX_HANDLERS = 4`).
- Provides `eth_send()`, which builds a frame around a caller-supplied payload (filling in our own MAC as the source), zero-pads it up to the 60-byte Ethernet minimum frame size if needed, and hands it to `e1000_send()`.

### Receive path: interrupt-driven, not polled

This kernel's scheduler is purely cooperative (`kernel/task.c`) — there is no timer-driven preemption, so a background "network daemon" task would only run whenever something else happened to call `task_yield()`, which is rare (mostly inside `waitpid`-style spin loops). A dedicated kernel task polling `e1000_receive()` would therefore starve for long stretches whenever the shell is just idle-halting on keyboard input.

Instead, `eth_init()` calls `e1000_set_rx_handler(eth_dispatch)` (`drivers/e1000.c`), so `eth_dispatch()` runs directly inside the NIC's RX interrupt handler (`e1000_irq()`) whenever the interrupt cause includes `RXT0` (frames ready) — exactly the same pattern `ata_irq()`/`keyboard_irq()` already use for doing real work inline rather than deferring it. `eth_dispatch()` drains the RX ring completely (calls `e1000_receive()` in a loop until it returns 0) before returning, so no frame is left pending when the interrupt handler exits.

Since this all happens with interrupts disabled (32-bit interrupt gates clear `IF` on entry — see `docs/interrupts.md`), and neither `e1000_send()` nor `e1000_receive()` ever block, running an ARP reply's `eth_send()` straight out of the RX interrupt handler is safe on this single-core, non-preemptive kernel.

### API

```c
void eth_init(void);
void eth_register_handler(uint16_t ethertype, eth_rx_handler_t handler);
const uint8_t *eth_get_mac(void);
int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len);
```

---

## ARP

**Source**: `net/arp.c`
**Header**: `include/pureunix/arp.h`

### Responsibilities

- Maintains a fixed-size (16-entry) IP-to-MAC cache with no eviction policy beyond "overwrite the first free slot, or slot 0 if full" — matching this kernel's general preference for small fixed-resource tables (e.g. `MAX_OPEN_FILES`).
- Registers with the Ethernet layer for `ETH_TYPE_ARP`; every ARP packet seen (request *or* reply) opportunistically updates the cache with its sender's IP/MAC pairing, exactly like a real ARP implementation.
- Answers "who-has" requests addressed to our own configured IP automatically, from interrupt context.
- `arp_resolve()` gives callers a synchronous-looking API over this asynchronous mechanism: it checks the cache, and on a miss, broadcasts a request and polls the cache (via `pit_sleep(10)`) until a timeout — the actual cache update happens asynchronously whenever the RX interrupt fires and `arp_input()` processes the reply.

### IPv4 address representation

`ip4_addr_t` (`include/pureunix/inet.h`, added in Phase 3) is a `uint32_t` in *host* byte order — `IP4_ADDR(192,168,1,1)` equals the "obvious" `0xC0A80101`. ARP originally (Phase 2) represented addresses as raw wire-order `uint8_t[4]` octet arrays instead, specifically to avoid picking a byte-order convention before any IP layer existed to need one; Phase 3 replaced that with `ip4_addr_t` throughout (`net/arp.c` converts to/from wire-order bytes only at the ARP packet's own `spa`/`tpa` fields, via `ip4_to_bytes()`/`ip4_from_bytes()`), since `net/ip.c`'s routing logic needs a real bitwise-AND-against-a-netmask, which a byte array can't do directly.

### Local IP configuration

`net/ip.c`'s `ip_configure(addr, netmask, gateway)` is the actual "simple network configuration" entry point — it sets the IP layer's own address/netmask/default-gateway and propagates the address to `arp_set_local_ip()` in one call. No DHCP exists yet, so `kernel_main()` (`kernel/main.c`) calls it with a hardcoded `10.0.2.15/24` via gateway `10.0.2.2` — the address QEMU's `-netdev user` (SLIRP) backend's built-in DHCP server would hand out to a real DHCP client, chosen so this host is reachable from (and can reach) that same virtual network without needing a real DHCP client.

### API

```c
void arp_set_local_ip(ip4_addr_t ip);
void arp_init(void);
bool arp_lookup(ip4_addr_t ip, uint8_t mac[6]);
void arp_request(ip4_addr_t ip);
bool arp_resolve(ip4_addr_t ip, uint8_t mac[6], uint32_t timeout_ms);
```

### Boot-time self-test

`arp_selftest()` (called from `kernel_main()` after `arch_enable_interrupts()`) does two things:

1. Attempts a real `arp_resolve()` against `10.0.2.2` (QEMU SLIRP's gateway address) — logs the result either way; a timeout here just means no reply arrived on this particular network backend, not necessarily a bug (see the e1000 driver's own self-test in `docs/drivers.md` for the same caveat, and the note on TX/RX verification below).
2. Feeds a synthetic ARP "who-has 10.0.2.15" request — from a locally-administered test MAC (`02:00:00:00:00:01`) and an RFC 5737 TEST-NET-1 address (`192.0.2.1`, reserved for documentation/testing so it can never collide with a real host) — directly into `arp_input()`, then checks that the reply path fired (a real `e1000_send()` call) and that the cache now resolves `192.0.2.1` back to that MAC. This proves the request/reply and cache logic correct independent of whether any real peer is reachable on the current network backend.

### Limitations

- No ARP cache expiry/TTL — entries persist until overwritten by a same-IP update or evicted to make room.
- No gratuitous ARP announcement on startup.
- No IPv6/NDP.
- Single Ethernet handler per ethertype (`ETH_MAX_HANDLERS = 4` total across all registered ethertypes).

---

## IPv4

**Source**: `net/ip.c`
**Header**: `include/pureunix/ip.h`

### Responsibilities

- Parses/builds the 20-byte IPv4 header (`ip4_header_t` — no options; outgoing packets always use IHL 5, and `ip_input()` tolerates a larger IHL on receive by skipping the options rather than interpreting them).
- Computes and verifies the header checksum (`ip_checksum()`, RFC 1071 Internet checksum — also reused as-is by `net/icmp.c`, since ICMP uses the identical algorithm over its own header+payload).
- **Fragment rejection**: any incoming datagram with the "more fragments" flag set or a non-zero fragment offset is silently dropped — there's no reassembly buffer, so half of a fragmented datagram is worse than none of it. A stricter stack might reply with an ICMP "fragmentation needed"; this one just drops.
- **Routing**: exactly one interface, so the entire routing table is one comparison — `(dst & netmask) == (local_addr & netmask)` sends directly to `dst`; anything else goes via the configured default gateway. Either way, the actual next-hop MAC comes from `arp_resolve()`.
- **Loopback interface**: `127.0.0.0/8` destinations (and datagrams addressed to our own configured address) are delivered straight back to the registered protocol handler inside `ip_send()` itself, never touching Ethernet or ARP — see the note on source-address handling below.
- Dispatches delivered datagrams to whichever handler (if any) is registered for its protocol number (`IP_PROTO_ICMP`, etc.), via a small fixed-size table (`IP_MAX_HANDLERS = 4`), the same pattern `net/eth.c` uses for ethertypes.

### Loopback source address

For a `127.0.0.0/8` destination, `ip_send()` stamps the *source* as the destination address itself (i.e. a `ping 127.0.0.1` looks like a genuine `127.0.0.1 <-> 127.0.0.1` conversation), not `local_addr` — this matters because `net/icmp.c`'s `icmp_ping()` matches an incoming echo reply against the exact address it pinged; stamping `local_addr` as the source instead would make a loopback ping's own reply invisible to the code waiting for it (a real bug hit and fixed during Phase 3 development — see the self-test below, which is exactly what caught it).

### Receive path

Registered with the Ethernet layer for `ETH_TYPE_IPV4` (`ip_init()`), so `ip_input()` runs in interrupt context for real traffic, same as `eth_dispatch()`/`arp_input()` — see the "Receive path: interrupt-driven, not polled" note in the Ethernet section above.

### API

```c
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

uint16_t ip_checksum(const void *data, size_t len);
void ip_register_handler(uint8_t protocol, ip_rx_handler_t handler);
void ip_configure(ip4_addr_t addr, ip4_addr_t netmask, ip4_addr_t gateway);
ip4_addr_t ip_local_addr(void);
void ip_init(void);
bool ip_resolve_route(ip4_addr_t dst, uint8_t mac[6], uint32_t timeout_ms);
int ip_send(ip4_addr_t dst, uint8_t protocol, const void *payload, uint16_t len);
```

### `ip_send()` never blocks -- a real deadlock, found and fixed

`ip_send()`'s ARP resolution is deliberately **non-blocking**: it only consults the ARP cache (`arp_lookup()`), and on a miss, fires an ARP request (`arp_request()`) and returns -1 immediately, rather than waiting for a reply. This is a direct consequence of a real bug found while hardening this phase: `ip_send()` originally called the *blocking* `arp_resolve()` unconditionally. Since `ip_send()` can run from inside the RX interrupt handler (via `icmp_input()`'s auto-reply path — see `net/eth.c`'s interrupt-driven receive path above), and interrupts are globally disabled for the duration of that handler (32-bit interrupt gates clear `IF` on entry — see `docs/interrupts.md`), calling the blocking `arp_resolve()` from there deadlocked the entire kernel solid: its `pit_sleep()`-based wait spins on `arch_halt()` until the PIT tick interrupt increments a counter, but that interrupt can never fire while nested inside another interrupt handler with `IF` cleared.

This was caught (not merely suspected) by constructing an independent test harness: a raw Ethernet-frame-over-UDP tunnel (`qemu -netdev socket,udp=...,localaddr=...`) that lets a host-side script inject arbitrary, fully-controlled frames into the guest's NIC, completely bypassing QEMU's usual SLIRP backend. Injecting a real ICMP echo request from an address outside the guest's configured subnet reliably reproduced the hang: the guest's log showed `icmp: echo request from <injected source>` and then nothing further — ever — until the QEMU process was killed. This is what "audit whether polling and interrupt paths behave identically" (an explicit part of this investigation) was looking for, and found: they did not. Every one of this project's own self-tests calls into the stack from normal (non-interrupt) context, where blocking is harmless, which is exactly why the bug went unnoticed through three phases of self-testing before an externally-injected frame surfaced it.

The fix: `ip_send()` itself never blocks (safe to call from anywhere, including interrupt context); callers in normal context that want reliable delivery to a destination that might not be ARP-cached yet should call the new `ip_resolve_route()` (blocking, same routing logic, callers' responsibility to only call it outside interrupt context) first to warm the cache -- see `net/icmp.c`'s `icmp_ping()` for the pattern. `ip_resolve_route()` also special-cases loopback destinations (returning immediately, matching `ip_send()`'s own loopback shortcut) so pinging `127.0.0.1` doesn't pointlessly try to resolve the default gateway.

### Limitations

- No fragmentation on send (`ip_send()` fails outright if `payload` doesn't fit one Ethernet frame) — matches the phase's stated scope of rejecting, not reassembling, fragments.
- No IP options support on send; tolerated (skipped) but not interpreted on receive.
- No TTL decrement/ICMP "time exceeded" — this isn't a router, so there's nothing to forward, but a real stack would still decrement and check TTL on delivery.
- Single default gateway/interface; no real routing table.
- `ip_send()`'s non-blocking cache-miss behavior means the *first* packet to any not-yet-resolved destination is always dropped (an ARP request is fired asynchronously instead) — standard, expected behavior for a minimal stack (real stacks do this too), but worth knowing if a single `ip_send()` call to a cold destination doesn't appear to do anything.

---

## ICMP

**Source**: `net/icmp.c`
**Header**: `include/pureunix/icmp.h`

### Responsibilities

- Answers incoming echo requests (type 8) addressed to us with an echo reply (type 0) — same identifier, sequence number, and payload, new checksum — automatically, from interrupt context for real traffic (mirroring ARP's automatic reply behavior).
- `icmp_ping()` gives callers a synchronous-looking "ping" API: sends an echo request, then polls (via `pit_sleep(10)`) for a matching reply (same id/seq/source) up to a timeout, filling in an approximate round-trip time on success. Only one ping can be outstanding at a time (a single "what am I waiting for" slot, not a table) — a limitation shared with `net/arp.c`'s single-cache-lookup design philosophy.

### API

```c
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
```

### Boot-time self-test

`icmp_selftest()` (called from `kernel_main()` after `arch_enable_interrupts()` and `ip_configure()`) does three things:

1. Pings `127.0.0.1` — a fully deterministic round trip through `ip_send()`'s loopback shortcut, exercising the complete encode → route → decode → auto-reply → decode-again path without touching Ethernet, ARP, or real hardware timing at all. This is the strongest, environment-independent proof that IPv4 + ICMP + checksums + loopback are all correct together.
2. Pings the configured default gateway (`10.0.2.2`) — best-effort, same caveat as `arp_selftest()`'s equivalent step (see "Environment note" below).
3. Feeds a synthetic incoming echo request (from the same RFC 5737 TEST-NET-1 test address `net/arp.c`'s self-test uses) directly into `icmp_input()`, mirroring `arp_selftest()`'s pattern — the resulting reply attempt is routed (the test address isn't in the local subnet) and therefore depends on gateway ARP resolution succeeding, so its outcome inherits the same environment caveat as step 2; the request-recognition and reply-construction logic itself is exercised regardless of whether the send ultimately succeeds.

### Limitations

- No ICMP error messages generated (destination unreachable, time exceeded, etc.) — only echo request/reply.
- Only one `icmp_ping()` call can be outstanding at a time.
- No `traceroute`-style TTL manipulation.

---

## Verifying with Wireshark

Since QEMU's `-netdev user` backend is a NAT (SLIRP) and isn't visible on the host's real network interfaces, capture at the point between the emulated NIC and the backend instead:

```
qemu-system-i386 ... -netdev user,id=net0 -device e1000,netdev=net0 \
    -object filter-dump,id=f1,netdev=net0,file=capture.pcap
```

Open `capture.pcap` in Wireshark after a clean shutdown (`quit` via the QEMU monitor, or graceful poweroff) to see the actual frames PureUNIX transmitted. For real host-visible traffic (e.g. to confirm another physical/virtual machine can ping or ARP-resolve this one), use a bridged or `tap` netdev instead of `user`.

## Resolved: real network reachability (root cause and fix)

Earlier revisions of this document concluded that "-netdev user" (SLIRP) never replying to PureUNIX was most likely a QEMU/environment characteristic outside the driver's control. **That conclusion was wrong.** It was a real, reproducible bug in this driver — found and fixed by comparing byte-for-byte against a known-good Linux guest under the *identical* QEMU configuration, which is what should have been done from the start rather than accepting the absence of a reply as inconclusive.

### The decisive comparison

Booting a stock Alpine Linux live ISO (`qemu-system-x86_64 -netdev user,id=net0 -device e1000,netdev=net0`, otherwise identical to PureUNIX's own test invocation) and running `udhcpc`, `arping 10.0.2.2`, and `ping -c3 1.1.1.1` from it succeeded perfectly — DHCP lease obtained, ARP resolved, both the gateway and a real external host (`1.1.1.1`, 0% packet loss) reachable. Repeating the exact same commands with DHCP skipped entirely (a bare static `10.0.2.15/24` assignment, mirroring PureUNIX's own configuration precisely) still worked — ruling out any DHCP-lease prerequisite. **This conclusively proved SLIRP works fine in this environment**, which meant the remaining gap had to be a genuine PureUNIX bug, not an environment limitation.

### Root cause: a compiler memory-ordering bug

A graceful (QEMU-monitor `quit`, not `SIGTERM`) packet capture of PureUNIX's own boot-time self-test traffic showed every single frame recorded with `caplen=0, origlen=0` — QEMU's own networking core genuinely believed PureUNIX was transmitting zero-length packets, even though the NIC's `TDH` register still advanced (the driver's own, and this document's earlier, evidence for "TX is correct"). Disassembling the compiled `e1000_send()` (`i686-elf-objdump -d`) found the actual bug:

```
mov DWORD PTR [eax+0x3818], ebx    ; reg_write(REG_TDT, tx_cur) -- doorbell: QEMU reads the
                                    ; descriptor from guest memory RIGHT NOW, synchronously
...
mov WORD PTR [ecx+0x8], dx         ; tx_descs[idx].length = len -- STILL HASN'T HAPPENED YET
```

GCC at `-O2` reordered the plain (non-`volatile`) descriptor field stores (`tx_descs[idx].length = len`, `.cmd = ...`, etc.) to occur *after* the `volatile` MMIO write that rings the transmit doorbell — even though the C source writes them in the opposite order. Nothing in the C standard's `volatile` semantics strictly prevents this (only accesses to *other* volatile objects are guaranteed to stay in program order relative to each other); GCC is conservative about this in most real-world code, but at `-O2`, for a tight sequence of independent non-aliasing stores immediately followed by one volatile write, it reordered them anyway.

The result: QEMU processed the doorbell write and read the descriptor's `length` field while it was still its BSS-zeroed initial value (`0`) — hardware doesn't validate `length > 0` before marking a descriptor `DD` (done), so the driver observed an apparently successful send (`TDH` advances) while a genuine zero-length frame went out. **This affected every single packet this driver ever transmitted, across every prior development phase** — explaining, in hindsight, every "no reply" result ever logged: there was never a real ARP request or ICMP echo on the wire to reply to. The identical bug pattern existed in `e1000_receive()` (the RX descriptor's `status = 0` clear reordered after the `REG_RDT` doorbell write) and in the one-time ring setup in `setup_rx()`/`setup_tx()`.

**The fix** (`drivers/e1000.c`): a `e1000_barrier()` helper — `__asm__ volatile("" ::: "memory")`, a compiler-only memory barrier (x86 doesn't reorder normal stores at the hardware level, so no actual fence instruction is needed) — called after writing descriptor fields and before the doorbell register write, in `e1000_send()`, `e1000_receive()`, `setup_rx()`, and `setup_tx()`.

### A second, related bug: `int $0x80` also masks interrupts

Exposing `icmp_ping()` to userspace via a new `SYS_PING` syscall (see `docs/syscalls.md`) hit the *same class* of bug a third time: `arch/i386/interrupt_stubs.S`'s `isr128` (the `int $0x80` handler) is a 32-bit interrupt gate like any other, so it enters with interrupts masked and only restores them on its own `iret`. `icmp_ping()` blocks on `pit_sleep()`, which needs the PIT tick interrupt to actually fire — calling it directly from the `SYS_PING` handler hung solid, identically to the RX-interrupt-context deadlock documented above for `ip_send()`. The fix (matching `drivers/tty.c`'s `tty_read()`, which already does this before its own `keyboard_getkey()`-based blocking wait): call `arch_enable_interrupts()` at the top of the `SYS_PING` handler, before calling `icmp_ping()`.

### Verified result

After both fixes, every self-test and packet capture confirms real, correct traffic:

```
arp: self-test resolved 10.0.2.2 -> 52:55:0a:00:02:02
icmp: self-test ping to gateway OK (0 ms)
```

`52:55:0a:00:02:02` is the exact same gateway MAC the Linux baseline resolved. A graceful packet capture at this point shows real, correctly-formed ARP request/reply and ICMP echo request/reply pairs (see `docs/drivers.md`'s e1000 section for the byte dump). The new `ping` command (`user/ping.c`, `/bin/ping` via `SYS_PING`) run interactively:

```
$ ping 10.0.2.2
PING 10.0.2.2
64 bytes from 10.0.2.2: seq=0 time=0ms
...
4 packets transmitted, 4 received, 0% packet loss
$ ping 1.1.1.1
PING 1.1.1.1
64 bytes from 1.1.1.1: seq=0 time=30ms
...
4 packets transmitted, 4 received, 0% packet loss
```

Both the local gateway and a real external host over the public internet (via SLIRP's own NAT) are reachable, with 0% packet loss, matching the Linux baseline exactly.

### Lesson

"No reply from an external peer" was, for three development phases, wrongly attributed to environment/QEMU behavior because every other piece of evidence available *from inside the guest* (TDH advancing, RX correctly processing externally-injected frames, multiple SLIRP configurations tried) was consistent with that story. The one thing that would have caught it immediately — comparing against a known-good guest under the identical configuration — wasn't done until asked for directly. Bugs that look environment-shaped from the inside are still worth a real baseline comparison before being written off.
