/*
 * user/ping.c — minimal ICMP echo ("ping") client.
 *
 * A thin CLI wrapper around SYS_PING (arch/i386/syscall.c), which itself
 * just calls net/icmp.c's icmp_ping() -- the kernel's networking stack
 * (drivers/e1000.c, net/eth.c, net/arp.c, net/ip.c, net/icmp.c) does all
 * the real work; this program only parses a dotted-quad address and
 * prints results in a familiar format. No raw sockets, no BSD sockets API
 * -- PureUNIX has neither yet, so this talks to the kernel's own ICMP
 * client directly via one syscall per echo request.
 */
#include "libpure.h"

#define PING_COUNT       4
#define PING_TIMEOUT_MS  2000

/* Parses "a.b.c.d" into a host-byte-order uint32 (IP4_ADDR(a,b,c,d)
 * convention, matching include/pureunix/inet.h). Returns 0 (and leaves
 * *out untouched) on a malformed address -- 0.0.0.0 is never a valid
 * ping target anyway, so this doubles as the failure sentinel. */
static int parse_ipv4(const char *s, unsigned int *out)
{
    unsigned int octets[4];
    for (int i = 0; i < 4; ++i) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        unsigned int value = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned int)(*s++ - '0');
            if (value > 255 || ++digits > 3) {
                return 0;
            }
        }
        octets[i] = value;
        if (i < 3) {
            if (*s != '.') {
                return 0;
            }
            ++s;
        }
    }
    if (*s != '\0') {
        return 0;
    }
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 1;
}

static void put_octet(unsigned int v)
{
    pu_puti((int)v);
}

static void put_ip(unsigned int ip)
{
    put_octet((ip >> 24) & 0xFF);
    pu_puts(".");
    put_octet((ip >> 16) & 0xFF);
    pu_puts(".");
    put_octet((ip >> 8) & 0xFF);
    pu_puts(".");
    put_octet(ip & 0xFF);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        pu_puts("usage: ping <ipv4-address>\n");
        return 1;
    }

    unsigned int dst;
    if (!parse_ipv4(argv[1], &dst)) {
        pu_puts("ping: invalid address '");
        pu_puts(argv[1]);
        pu_puts("'\n");
        return 1;
    }

    pu_puts("PING ");
    put_ip(dst);
    pu_puts("\n");

    int sent = 0;
    int received = 0;
    for (int seq = 0; seq < PING_COUNT; ++seq) {
        ++sent;
        unsigned int rtt_ms = 0;
        int rc = pu_ping(dst, PING_TIMEOUT_MS, &rtt_ms);
        if (rc == 0) {
            ++received;
            pu_puts("64 bytes from ");
            put_ip(dst);
            pu_puts(": seq=");
            pu_puti(seq);
            pu_puts(" time=");
            pu_puti((int)rtt_ms);
            pu_puts("ms\n");
        } else {
            pu_puts("Request timeout for seq ");
            pu_puti(seq);
            pu_puts("\n");
        }
    }

    pu_puts("\n--- ");
    put_ip(dst);
    pu_puts(" ping statistics ---\n");
    pu_puti(sent);
    pu_puts(" packets transmitted, ");
    pu_puti(received);
    pu_puts(" received, ");
    pu_puti(sent ? (sent - received) * 100 / sent : 0);
    pu_puts("% packet loss\n");

    return received > 0 ? 0 : 1;
}
