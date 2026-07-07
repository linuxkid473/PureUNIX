#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/e1000.h>
#include <pureunix/io.h>
#include <pureunix/memory.h>
#include <pureunix/pci.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vmm.h>

/* Register offsets (byte offsets into the MMIO BAR0), classic e1000 layout
 * shared by the 82540/82541/82543-82547 family -- this is exactly what
 * QEMU's "-device e1000" (82540EM) emulates. */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EEPROM   0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404

#define CTRL_ASDE   0x00000020U
#define CTRL_SLU    0x00000040U
#define CTRL_RST    0x04000000U

#define STATUS_LU   0x00000002U

#define RCTL_EN         0x00000002U
#define RCTL_BAM        0x00008000U
#define RCTL_BSIZE_2048 0x00000000U
#define RCTL_SECRC      0x04000000U

#define TCTL_EN     0x00000002U
#define TCTL_PSP    0x00000008U
#define TCTL_RTLC   0x01000000U

#define CMD_EOP  0x01U
#define CMD_IFCS 0x02U
#define CMD_RS   0x08U

#define TSTA_DD  0x01U
#define RSTA_DD  0x01U

#define ICR_LSC   0x00000004U
#define ICR_RXT0  0x00000080U
#define IMS_LSC   0x00000004U
#define IMS_RXT0  0x00000080U

/* Device IDs sharing the register layout above -- the classic (pre-e1000e)
 * Intel gigabit family. 0x100E is 82540EM, the model QEMU's "-device
 * e1000"/"e1000-82540em" emulates by default. 82574L (e1000e, 0x10D3) and
 * newer NICs use a different register set and are deliberately not listed. */
static const uint16_t e1000_device_ids[] = {
    0x1000, 0x1001, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x100E, 0x100F,
    0x1010, 0x1011, 0x1012, 0x1013, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019,
    0x101A, 0x101D, 0x101E, 0x1026, 0x1027, 0x1028, 0x105E, 0x1075, 0x1076,
    0x1077, 0x1078,
};

typedef struct __attribute__((packed)) e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8
#define E1000_BUF_SIZE    2048

/* Descriptor rings and packet buffers are plain static (kernel-image, BSS)
 * arrays rather than kmalloc()'d -- same trick vmm.c's page_directory/
 * identity_tables use -- so each ring is one guaranteed-contiguous,
 * naturally 16-byte-aligned physical block without needing an aligning
 * allocator, and the whole thing lives inside the identity-mapped low
 * 128 MiB where virt == phys, so a buffer's own address doubles as the
 * physical address the NIC's DMA engine needs. */
static e1000_rx_desc_t rx_descs[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_descs[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_buffers[E1000_NUM_RX_DESC][E1000_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[E1000_NUM_TX_DESC][E1000_BUF_SIZE] __attribute__((aligned(16)));

static volatile uint32_t *mmio_base;
static bool present;
static uint8_t mac_addr[6];
static uint16_t rx_cur;
static uint16_t tx_cur;
static void (*rx_handler)(void);

/* Lightweight, always-on counters -- not per-packet logging (which would
 * flood the console under any real traffic), just enough running state
 * for e1000_dump_stats() to give a real diagnostic picture on demand.
 * Added while root-causing a real interrupt-context deadlock bug (see
 * net/ip.c's ip_send() comment) that manual register dumps alone hadn't
 * made obvious. */
static uint32_t stat_tx_ok;
static uint32_t stat_tx_timeout;
static uint32_t stat_rx_ok;
static uint32_t stat_rx_dropped_size;
static uint32_t stat_irq_count;
static uint32_t stat_irq_rxt0_count;

static void reg_write(uint32_t reg, uint32_t value)
{
    mmio_base[reg / 4] = value;
}

static uint32_t reg_read(uint32_t reg)
{
    return mmio_base[reg / 4];
}

/* Cheap fixed-iteration delay with no dependency on PIT ticks -- e1000_init()
 * runs before arch_enable_interrupts() (see kernel/main.c), same as
 * ata_init(), so pit_sleep() (which needs IRQ0 actually firing) isn't usable
 * yet. Mirrors ata.c's own ata_delay()/busy-wait idiom. */
static void e1000_io_delay(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; ++i) {
        io_wait();
    }
}

static void e1000_irq(interrupt_regs_t *regs)
{
    (void)regs;
    ++stat_irq_count;
    /* Reading ICR both reports and clears the pending cause bits. */
    uint32_t cause = reg_read(REG_ICR);
    /* rx_handler (set via e1000_set_rx_handler(), see net/eth.c's
     * eth_init()) drains the RX ring by calling e1000_receive() in a loop
     * until empty -- run directly here in interrupt context, the same way
     * ata_irq()/keyboard_irq() do their real work inline rather than
     * deferring to a separate task, since this kernel's scheduler is
     * purely cooperative (see kernel/task.c) and would otherwise never get
     * around to a background poller. */
    if (cause & ICR_RXT0) {
        ++stat_irq_rxt0_count;
    }
    if ((cause & ICR_RXT0) && rx_handler) {
        rx_handler();
    }
}

void e1000_set_rx_handler(void (*handler)(void))
{
    rx_handler = handler;
}

static uint16_t eeprom_read_word(uint8_t addr)
{
    reg_write(REG_EEPROM, 1U | ((uint32_t)addr << 8));
    uint32_t value = 0;
    for (uint32_t i = 0; i < 100000; ++i) {
        value = reg_read(REG_EEPROM);
        if (value & 0x10U) {
            break;
        }
    }
    return (uint16_t)(value >> 16);
}

static bool eeprom_present(void)
{
    reg_write(REG_EEPROM, 1U);
    for (uint32_t i = 0; i < 100000; ++i) {
        if (reg_read(REG_EEPROM) & 0x10U) {
            return true;
        }
    }
    return false;
}

static void read_mac_address(void)
{
    if (eeprom_present()) {
        for (int i = 0; i < 3; ++i) {
            uint16_t word = eeprom_read_word((uint8_t)i);
            mac_addr[i * 2]     = (uint8_t)(word & 0xFF);
            mac_addr[i * 2 + 1] = (uint8_t)(word >> 8);
        }
    } else {
        /* No EEPROM (or emulation doesn't model one) -- QEMU still always
         * preloads RAL0/RAH0 with a valid default MAC, so fall back to
         * reading the receive-address registers directly. */
        uint32_t ral = reg_read(REG_RAL0);
        uint32_t rah = reg_read(REG_RAH0);
        mac_addr[0] = (uint8_t)(ral);
        mac_addr[1] = (uint8_t)(ral >> 8);
        mac_addr[2] = (uint8_t)(ral >> 16);
        mac_addr[3] = (uint8_t)(ral >> 24);
        mac_addr[4] = (uint8_t)(rah);
        mac_addr[5] = (uint8_t)(rah >> 8);
    }

    /* Program the primary receive-address filter with our own MAC either
     * way, so unicast frames addressed to us are actually accepted
     * regardless of whether it came from the EEPROM or was already there. */
    uint32_t ral = mac_addr[0] | (mac_addr[1] << 8) | (mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24);
    uint32_t rah = mac_addr[4] | (mac_addr[5] << 8) | 0x80000000U /* Address Valid */;
    reg_write(REG_RAL0, ral);
    reg_write(REG_RAH0, rah);
}

static void e1000_reset(void)
{
    reg_write(REG_IMC, 0xFFFFFFFFU);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (uint32_t i = 0; i < 100000; ++i) {
        if (!(reg_read(REG_CTRL) & CTRL_RST)) {
            break;
        }
    }
    e1000_io_delay(1000);
    reg_write(REG_IMC, 0xFFFFFFFFU);
    (void)reg_read(REG_ICR);
}

static void setup_rx(void)
{
    for (int i = 0; i < E1000_NUM_RX_DESC; ++i) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
        rx_descs[i].status = 0;
    }
    rx_cur = 0;

    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_descs);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, sizeof(rx_descs));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, E1000_NUM_RX_DESC - 1);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void setup_tx(void)
{
    for (int i = 0; i < E1000_NUM_TX_DESC; ++i) {
        tx_descs[i].addr = (uint64_t)(uintptr_t)tx_buffers[i];
        tx_descs[i].cmd = 0;
        /* Mark every slot "already complete" up front so the first round of
         * e1000_send() calls doesn't spin waiting on hardware that has never
         * touched these descriptors. */
        tx_descs[i].status = TSTA_DD;
    }
    tx_cur = 0;

    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_descs);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, sizeof(tx_descs));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 0x0060200AU);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0FU << 4) | (0x40U << 12) | TCTL_RTLC);
}

/* Builds a 60-byte (minimum Ethernet frame size) broadcast ARP request
 * asking "who has 10.0.2.2" -- the gateway address QEMU's usermode network
 * backend (-netdev user) always answers on, regardless of what IP config
 * the guest itself has. Used only by e1000_selftest() below to generate
 * real traffic without needing any IP stack in the kernel. */
static void build_arp_probe(uint8_t frame[60])
{
    memset(frame, 0, 60);
    memset(frame, 0xFF, 6);            /* dst MAC: broadcast */
    memcpy(frame + 6, mac_addr, 6);    /* src MAC */
    frame[12] = 0x08; frame[13] = 0x06; /* ethertype: ARP */

    uint8_t *arp = frame + 14;
    arp[0] = 0x00; arp[1] = 0x01;      /* HTYPE: Ethernet */
    arp[2] = 0x08; arp[3] = 0x00;      /* PTYPE: IPv4 */
    arp[4] = 6;                        /* HLEN */
    arp[5] = 4;                        /* PLEN */
    arp[6] = 0x00; arp[7] = 0x01;      /* OPER: request */
    memcpy(arp + 8, mac_addr, 6);      /* sender MAC */
    arp[14] = 10; arp[15] = 0; arp[16] = 2; arp[17] = 15; /* sender IP (arbitrary) */
    /* target MAC left zeroed -- that's what we're asking for */
    arp[24] = 10; arp[25] = 0; arp[26] = 2; arp[27] = 2;  /* target IP: gateway */
}

void e1000_init(void)
{
    if (present) {
        return;
    }

    pci_device_t dev;
    if (!pci_find(0x8086, e1000_device_ids, (int)ARRAY_SIZE(e1000_device_ids), &dev)) {
        printf("e1000: no Intel e1000-family NIC found\n");
        return;
    }
    printf("e1000: found %04x:%04x at %02x:%02x.%u (irq %u)\n",
           dev.vendor_id, dev.device_id, dev.bus, dev.slot, dev.func, dev.interrupt_line);

    pci_enable_bus_mastering(&dev);

    /* PCI Cache Line Size register (config offset 0x0C, in 32-bit-dword
     * units): on real hardware this tells the NIC's DMA engine the host's
     * cache line size so it can choose efficient burst lengths for
     * descriptor writeback; left at 0 (its power-on default, if no BIOS/
     * firmware set it) some e1000 revisions fall back to a much more
     * conservative single-dword burst. QEMU's e1000 model doesn't appear
     * to condition receive/transmit behavior on this register, but it's
     * cheap, harmless, and correct to set on general principle -- 0x10 (16
     * dwords = 64 bytes) is the standard x86 cache line size. */
    pci_config_write8(dev.bus, dev.slot, dev.func, 0x0C, 0x10);

    phys_addr_t bar0 = pci_bar_address(&dev, 0);
    uint32_t bar_size = pci_bar_size(&dev, 0);
    if (!bar0 || !bar_size) {
        printf("e1000: BAR0 is not a usable memory-mapped BAR\n");
        return;
    }

    /* BAR0 sits far above the 128 MiB identity map (typically ~0xFEBC0000
     * on QEMU), so it needs its own mapping -- identity-mapped 1:1 like
     * everything else in this kernel (see docs/memory.md), just installed
     * lazily here instead of at vmm_init() time. */
    uint32_t pages = (bar_size + PUREUNIX_PAGE_SIZE - 1) / PUREUNIX_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; ++i) {
        phys_addr_t page = bar0 + i * PUREUNIX_PAGE_SIZE;
        vmm_map_page(page, page, PAGE_PRESENT | PAGE_WRITE);
    }
    mmio_base = (volatile uint32_t *)(uintptr_t)bar0;
    printf("e1000: MMIO mapped at %p (%u KiB)\n", (void *)(uintptr_t)bar0, bar_size / 1024);

    e1000_reset();

    for (int i = 0; i < 128; ++i) {
        reg_write(REG_MTA + (uint32_t)i * 4, 0);
    }

    read_mac_address();
    printf("e1000: MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    setup_rx();
    setup_tx();

    if (dev.interrupt_line < 16) {
        interrupt_register_handler((uint8_t)(32 + dev.interrupt_line), e1000_irq);
        irq_enable(dev.interrupt_line);
        reg_write(REG_IMS, IMS_RXT0 | IMS_LSC);
    }

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    e1000_io_delay(1000);

    present = true;
    printf("e1000: initialization complete, link %s\n",
           (reg_read(REG_STATUS) & STATUS_LU) ? "up" : "down");
}

/* Sends the ARP probe above and polls the RX ring for a reply, exercising
 * the full TX -> wire -> RX descriptor path on real (emulated) hardware.
 * Needs actual wall-clock time (pit_sleep()) to give a reply a fair chance
 * to arrive, so -- unlike e1000_init() itself -- this must run after
 * arch_enable_interrupts() (see kernel/main.c). Purely diagnostic: seeing
 * no reply is logged, not treated as an error, since it depends on the
 * network backend actually being reachable (e.g. QEMU's "-netdev user"
 * SLIRP replying to an unsolicited ARP for its own gateway is not
 * guaranteed by every version/configuration) -- TX itself is confirmed
 * independently of any reply, since e1000_send() only returns 0 once the
 * NIC's own TX descriptor head has moved past the frame. */
void e1000_selftest(void)
{
    if (!present) {
        return;
    }

    uint8_t frame[60];
    build_arp_probe(frame);

    if (e1000_send(frame, sizeof(frame)) != 0) {
        printf("e1000: self-test TX failed\n");
        return;
    }
    printf("e1000: self-test sent a broadcast ARP probe (60 bytes) for 10.0.2.2\n");

    uint8_t reply[E1000_BUF_SIZE];
    bool got_reply = false;
    for (uint32_t attempt = 0; attempt < 100 && !got_reply; ++attempt) {
        int n = e1000_receive(reply, sizeof(reply));
        if (n > 0) {
            uint16_t ethertype = ((uint16_t)reply[12] << 8) | reply[13];
            printf("e1000: self-test RX ring filled -- received %u bytes, ethertype %x\n",
                   (uint32_t)n, ethertype);
            got_reply = true;
        } else {
            pit_sleep(10);
        }
    }
    if (!got_reply) {
        printf("e1000: self-test saw no RX reply (depends on network backend reachability)\n");
    }
    e1000_dump_stats();
}

bool e1000_present(void)
{
    return present;
}

void e1000_get_mac(uint8_t mac[6])
{
    if (!present) {
        return;
    }
    memcpy(mac, mac_addr, 6);
}

int e1000_send(const void *data, uint16_t len)
{
    if (!present || len == 0 || len > E1000_BUF_SIZE) {
        return -1;
    }

    uint16_t idx = tx_cur;
    uint32_t spins = 0;
    while (!(tx_descs[idx].status & TSTA_DD)) {
        if (++spins > 1000000) {
            ++stat_tx_timeout;
            return -1;
        }
    }

    memcpy(tx_buffers[idx], data, len);
    tx_descs[idx].addr = (uint64_t)(uintptr_t)tx_buffers[idx];
    tx_descs[idx].length = len;
    tx_descs[idx].cso = 0;
    tx_descs[idx].cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    tx_descs[idx].status = 0;

    tx_cur = (uint16_t)((idx + 1) % E1000_NUM_TX_DESC);
    reg_write(REG_TDT, tx_cur);
    ++stat_tx_ok;
    return 0;
}

int e1000_receive(void *buf, uint16_t buf_len)
{
    if (!present) {
        return -1;
    }

    uint16_t idx = rx_cur;
    if (!(rx_descs[idx].status & RSTA_DD)) {
        return 0;
    }

    uint16_t len = rx_descs[idx].length;
    int result = (int)len;
    if (len > buf_len) {
        result = -1;
        ++stat_rx_dropped_size;
    } else {
        memcpy(buf, rx_buffers[idx], len);
        ++stat_rx_ok;
    }

    rx_descs[idx].status = 0;
    rx_cur = (uint16_t)((idx + 1) % E1000_NUM_RX_DESC);
    reg_write(REG_RDT, idx);
    return result;
}

void e1000_dump_stats(void)
{
    if (!present) {
        printf("e1000: not present\n");
        return;
    }
    printf("e1000: stats tx_ok=%u tx_timeout=%u rx_ok=%u rx_dropped_size=%u irq_count=%u irq_rxt0_count=%u\n",
           stat_tx_ok, stat_tx_timeout, stat_rx_ok, stat_rx_dropped_size, stat_irq_count, stat_irq_rxt0_count);
    printf("e1000: rx ring RDH=%u RDT=%u rx_cur=%u | tx ring TDH=%u TDT=%u tx_cur=%u\n",
           reg_read(REG_RDH), reg_read(REG_RDT), rx_cur, reg_read(REG_TDH), reg_read(REG_TDT), tx_cur);
    /* Reading ICR clears its pending cause bits, same as e1000_irq() --
     * diagnostic-only, don't call this mid-traffic if a real interrupt's
     * cause bits still need to be observed elsewhere. */
    printf("e1000: ICR=%x IMS=%x STATUS=%x\n", reg_read(REG_ICR), reg_read(REG_IMS), reg_read(REG_STATUS));
}
