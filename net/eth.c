#include <pureunix/byteorder.h>
#include <pureunix/e1000.h>
#include <pureunix/eth.h>
#include <pureunix/string.h>

#define ETH_MAX_HANDLERS 4
#define ETH_MIN_FRAME    60
#define ETH_MAX_FRAME    1514 /* 1500 MTU + 14-byte header */

const uint8_t eth_broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct eth_handler_entry {
    uint16_t ethertype;
    eth_rx_handler_t handler;
} eth_handler_entry_t;

static eth_handler_entry_t handlers[ETH_MAX_HANDLERS];
static int handler_count;

/* Drains the RX ring: e1000_set_rx_handler() (see e1000_init() below)
 * points the NIC driver's RX interrupt straight at this function, so it
 * runs in interrupt context every time the hardware reports frames ready
 * -- see the comment on e1000_irq() in drivers/e1000.c for why this kernel
 * does real work inline in IRQ handlers instead of deferring to a
 * scheduled task. */
static void eth_dispatch(void)
{
    static uint8_t frame[ETH_MAX_FRAME];
    for (;;) {
        int n = e1000_receive(frame, sizeof(frame));
        if (n <= 0) {
            return;
        }
        if (n < (int)sizeof(eth_header_t)) {
            continue;
        }
        const eth_header_t *hdr = (const eth_header_t *)frame;
        uint16_t ethertype = ntohs(hdr->ethertype);
        const uint8_t *payload = frame + sizeof(eth_header_t);
        uint16_t payload_len = (uint16_t)(n - (int)sizeof(eth_header_t));
        for (int i = 0; i < handler_count; ++i) {
            if (handlers[i].ethertype == ethertype) {
                handlers[i].handler(hdr->src, payload, payload_len);
            }
        }
    }
}

void eth_init(void)
{
    handler_count = 0;
    e1000_set_rx_handler(eth_dispatch);
}

void eth_register_handler(uint16_t ethertype, eth_rx_handler_t handler)
{
    if (handler_count < ETH_MAX_HANDLERS) {
        handlers[handler_count].ethertype = ethertype;
        handlers[handler_count].handler = handler;
        ++handler_count;
    }
}

const uint8_t *eth_get_mac(void)
{
    static uint8_t mac[ETH_ALEN];
    e1000_get_mac(mac);
    return mac;
}

int eth_send(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len)
{
    if (len > ETH_MAX_FRAME - sizeof(eth_header_t)) {
        return -1;
    }

    uint8_t frame[ETH_MAX_FRAME];
    eth_header_t *hdr = (eth_header_t *)frame;
    memcpy(hdr->dst, dst_mac, ETH_ALEN);
    memcpy(hdr->src, eth_get_mac(), ETH_ALEN);
    hdr->ethertype = htons(ethertype);
    memcpy(frame + sizeof(eth_header_t), payload, len);

    uint16_t total = (uint16_t)(sizeof(eth_header_t) + len);
    if (total < ETH_MIN_FRAME) {
        memset(frame + total, 0, ETH_MIN_FRAME - total);
        total = ETH_MIN_FRAME;
    }
    return e1000_send(frame, total);
}
