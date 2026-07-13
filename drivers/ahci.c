/* drivers/ahci.c -- native AHCI (SATA) driver.
 *
 * Real motivation: drivers/ata.c only ever probes the legacy IDE I/O ports
 * (0x1F0/0x3F6) -- a real SATA-only board (no PATA controller silicon at
 * all, the norm for anything built in roughly the last ~15 years) has
 * nothing listening there, so ata_init() reports both master and slave as
 * !present and kernel/main.c's find_persistent_root_disk() has no way to
 * reach that board's actual on-disk EXT2 root even though the disk itself
 * is perfectly readable -- just not through the interface this kernel used
 * to assume every disk speaks. This driver adds the interface real modern
 * SATA hardware (and QEMU's own `-M q35 -device ahci`) actually exposes:
 * a single memory-mapped PCI BAR (AHCI 1.3.1 spec), one port per physical
 * SATA connector, each port driven by a tiny command list + FIS receive
 * area + command table, all DMA'd directly rather than the legacy PIO
 * word-at-a-time transfer ata.c uses.
 *
 * Scope: single-sector LBA48 DMA read/write per disk_device_t call (no
 * multi-sector batching, no NCQ) -- exactly matching every other
 * disk_device_t in this kernel (ata.c, ramdisk.c, usb_msd.c), all of which
 * are called one 512-byte sector at a time by fs/ext2 and fs/fat16. Command
 * slot 0 only, one command in flight at a time per port -- a boot-time root
 * filesystem driver has no need for the queuing AHCI/NCQ exists to provide.
 */
#include <pureunix/ahci.h>
#include <pureunix/memory.h>
#include <pureunix/pci.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vmm.h>

/* One 4096-byte page per port, laid out as:
 *   [0x000, 0x400)  Command List  (32 entries x 32 bytes -- only entry 0
 *                    is ever filled in; the HBA never looks past PxCI's
 *                    highest set bit, which this driver never sets above
 *                    bit 0)
 *   [0x400, 0x500)  FIS Receive Area (256 bytes, must be 256-byte aligned
 *                    -- 0x400 satisfies that)
 *   [0x500, 0x600)  Command Table for slot 0 (must be 128-byte aligned --
 *                    0x500 = 1280 = 10*128 satisfies that; 256 bytes is far
 *                    more than the 128-byte CFIS/ACMD/reserved region plus
 *                    one 16-byte PRDT entry this driver ever needs) */
#define AHCI_PAGE_CMDLIST_OFFSET  0x000U
#define AHCI_PAGE_FIS_OFFSET      0x400U
#define AHCI_PAGE_CMDTABLE_OFFSET 0x500U

#define AHCI_POLL_ITERATIONS 1000000U

typedef struct {
    bool present;
    uint32_t port_index; /* 0-based hardware port number (PI bitmap index) */
    volatile uint8_t *port_regs; /* mmio + AHCI_PORT_REGS_BASE + port_index*AHCI_PORT_STRIDE */
    uint8_t *page; /* identity-mapped (virt == phys) DMA page, see layout above */
    phys_addr_t page_phys;
    disk_device_t disk;
} ahci_port_ctx_t;

static struct {
    bool present;
    volatile uint8_t *mmio;
    pci_device_t pci;
} ctrl;

static ahci_port_ctx_t ports[AHCI_MAX_DISKS];

static uint32_t reg_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void reg_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

/* Compiler-only memory barrier -- same reasoning as xhci_barrier()/
 * e1000_barrier(): the command header/table are plain structs the compiler
 * is otherwise free to reorder relative to the PxCI doorbell write; x86
 * doesn't reorder stores at the hardware level, so nothing stronger than
 * this is required. */
static inline void ahci_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Maps every page the BAR spans into the kernel's identity-mapped address
 * space (virt == phys) -- identical in shape to drivers/xhci.c's map_bar(),
 * which see for why vmm_map_mmio_uc() specifically (strict Uncacheable,
 * not merely whatever a neighboring region's paging attributes happen to
 * leave it as). */
static volatile uint8_t *map_bar(phys_addr_t base, uint32_t size)
{
    uint32_t pages = (size + PUREUNIX_PAGE_SIZE - 1) / PUREUNIX_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; ++i) {
        phys_addr_t page = base + i * PUREUNIX_PAGE_SIZE;
        vmm_map_mmio_uc(page, page, PAGE_WRITE);
    }
    return (volatile uint8_t *)(uintptr_t)base;
}

/* Allocates one zeroed, page-aligned physical frame for a port's DMA
 * structures. pmm_alloc_frame() frames come from the identity-mapped low
 * 128MiB (kernel/pmm.c), so virt == phys always, exactly like
 * drivers/xhci.c's alloc_dma_page(). */
static uint8_t *alloc_dma_page(phys_addr_t *out_phys)
{
    phys_addr_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    uint8_t *ptr = (uint8_t *)(uintptr_t)frame;
    memset(ptr, 0, PUREUNIX_PAGE_SIZE);
    *out_phys = frame;
    return ptr;
}

/* Stops a port's command list/FIS-receive engines (AHCI sec 10.3.1) before
 * this driver reprograms PxCLB/PxFB -- required even though nothing should
 * have started the port yet on a from-reset boot, because a real BIOS/UEFI
 * (and, on real hardware, sometimes GRUB's own AHCI probing) may have left
 * a port running from its own earlier use. Reprogramming PxCLB/PxFB while
 * the engines are still running is explicitly undefined by the spec, not
 * just untidy. Best-effort: logs and proceeds either way, since a timeout
 * here (engine wedged) will simply surface again, more concretely, when
 * this driver tries to actually issue a command later. */
static void port_stop(volatile uint8_t *p)
{
    uint32_t cmd = reg_read32(p, AHCI_PXCMD);
    if (!(cmd & (AHCI_PXCMD_ST | AHCI_PXCMD_FRE))) {
        return;
    }

    reg_write32(p, AHCI_PXCMD, cmd & ~AHCI_PXCMD_ST);
    bool cr_clear = false;
    for (uint32_t i = 0; i < AHCI_POLL_ITERATIONS; ++i) {
        if (!(reg_read32(p, AHCI_PXCMD) & AHCI_PXCMD_CR)) {
            cr_clear = true;
            break;
        }
    }
    if (!cr_clear) {
        printf("ahci: port stop: PxCMD.CR did not clear -- proceeding anyway\n");
    }

    cmd = reg_read32(p, AHCI_PXCMD);
    reg_write32(p, AHCI_PXCMD, cmd & ~AHCI_PXCMD_FRE);
    bool fr_clear = false;
    for (uint32_t i = 0; i < AHCI_POLL_ITERATIONS; ++i) {
        if (!(reg_read32(p, AHCI_PXCMD) & AHCI_PXCMD_FR)) {
            fr_clear = true;
            break;
        }
    }
    if (!fr_clear) {
        printf("ahci: port stop: PxCMD.FR did not clear -- proceeding anyway\n");
    }
}

/* Brings up one port's command engine after ahci_init() has already
 * confirmed a real SATA disk answers it (PxSSTS.DET==3, PxSIG==ATA):
 * allocates its DMA page, points PxCLB/PxFB at the Command List/FIS Receive
 * Area within it, then enables FIS Receive (FRE) and the command list (ST),
 * in that order -- the order the spec requires (sec 10.1.2), since the
 * device's initial D2H Register FIS after COMRESET is only captured if FRE
 * is already enabled by the time ST starts the engine. */
static bool port_start(ahci_port_ctx_t *ctx)
{
    volatile uint8_t *p = ctx->port_regs;
    port_stop(p);

    ctx->page = alloc_dma_page(&ctx->page_phys);
    if (!ctx->page) {
        printf("ahci: port %u: failed to allocate DMA page\n", ctx->port_index);
        return false;
    }

    reg_write32(p, AHCI_PXCLB, (uint32_t)(ctx->page_phys + AHCI_PAGE_CMDLIST_OFFSET));
    reg_write32(p, AHCI_PXCLBU, 0);
    reg_write32(p, AHCI_PXFB, (uint32_t)(ctx->page_phys + AHCI_PAGE_FIS_OFFSET));
    reg_write32(p, AHCI_PXFBU, 0);

    /* SERR is RW1C -- write back whatever is currently set to clear any
     * stale error bits left over from the pre-boot/BIOS handoff before this
     * driver starts issuing commands of its own. */
    reg_write32(p, AHCI_PXSERR, reg_read32(p, AHCI_PXSERR));

    uint32_t cmd = reg_read32(p, AHCI_PXCMD);
    reg_write32(p, AHCI_PXCMD, cmd | AHCI_PXCMD_FRE);
    cmd = reg_read32(p, AHCI_PXCMD);
    reg_write32(p, AHCI_PXCMD, cmd | AHCI_PXCMD_ST);
    return true;
}

/* Issues one H2D Register FIS command on slot 0 and polls (bounded) for
 * completion, exactly mirroring drivers/ata.c's own polled-PIO wait loops
 * in spirit even though the transport underneath is entirely different
 * (DMA descriptor + doorbell vs. word-at-a-time PIO). `buf`/`sector_count`
 * are ignored for a non-data command (FLUSH CACHE EXT) -- pass NULL/0.
 * `buf` must be a plain kernel pointer within the identity-mapped low
 * 128MiB (true for every disk_device_t caller in this kernel -- fs/ext2 and
 * fs/fat16 both pass stack or kmalloc()'d buffers), since its virtual
 * address is used directly as the PRDT's physical data address with no
 * translation, exactly like drivers/xhci.c's control_transfer() does for
 * its own DMA buffers. */
static bool ahci_issue_command(ahci_port_ctx_t *ctx, uint8_t command, uint32_t lba, uint8_t *buf,
                                bool is_write)
{
    volatile uint8_t *p = ctx->port_regs;
    uint8_t *cmdlist = ctx->page + AHCI_PAGE_CMDLIST_OFFSET;
    uint8_t *cmdtable = ctx->page + AHCI_PAGE_CMDTABLE_OFFSET;
    memset(cmdtable, 0, 128); /* CFIS + ACMD + reserved */

    uint8_t *cfis = cmdtable;
    cfis[0] = AHCI_FIS_TYPE_REG_H2D;
    cfis[1] = 0x80U; /* C (Command) bit set, PM Port 0 */
    cfis[2] = command;
    cfis[3] = 0; /* Features (7:0) */
    cfis[4] = (uint8_t)(lba & 0xFFU);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFFU);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFFU);
    cfis[7] = 0x40U; /* Device: LBA mode */
    cfis[8] = (uint8_t)((lba >> 24) & 0xFFU);
    cfis[9] = 0; /* LBA[39:32] -- always 0, this driver only ever addresses a
                  * single 32-bit LBA parameter (disk_device_t's contract) */
    cfis[10] = 0; /* LBA[47:40] */
    cfis[11] = 0; /* Features (15:8) */
    cfis[12] = buf ? 1U : 0U; /* Sector Count (7:0) -- 1 sector, or 0 for a
                                * non-data command like FLUSH CACHE EXT */
    cfis[13] = 0; /* Sector Count (15:8) */

    uint32_t prdtl = 0;
    if (buf) {
        uint8_t *prdt = cmdtable + 0x80U;
        uint32_t data_phys = (uint32_t)(uintptr_t)buf;
        *(volatile uint32_t *)(prdt + 0) = data_phys;
        *(volatile uint32_t *)(prdt + 4) = 0;
        *(volatile uint32_t *)(prdt + 8) = 0;
        *(volatile uint32_t *)(prdt + 12) = 511U; /* Data Byte Count - 1 (one 512-byte sector) */
        prdtl = 1;
    }

    uint32_t *hdr = (uint32_t *)cmdlist; /* slot 0, 8 dwords */
    hdr[0] = 5U /* CFL: H2D Register FIS is 20 bytes = 5 dwords */
        | (is_write ? (1U << 6) : 0U) /* W: 1 = Host->Device (write) */
        | (prdtl << 16);
    hdr[1] = 0; /* PRDBC, set by the HBA */
    hdr[2] = (uint32_t)(uintptr_t)cmdtable;
    hdr[3] = 0;
    hdr[4] = 0;
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0;

    /* SERR/IS are RW1C -- clear any stale bits before issuing so a
     * subsequent error check only reflects *this* command. */
    reg_write32(p, AHCI_PXSERR, reg_read32(p, AHCI_PXSERR));
    reg_write32(p, AHCI_PXIS, reg_read32(p, AHCI_PXIS));

    ahci_barrier();
    reg_write32(p, AHCI_PXCI, 1U << 0);

    bool done = false;
    for (uint32_t i = 0; i < AHCI_POLL_ITERATIONS; ++i) {
        uint32_t tfd = reg_read32(p, AHCI_PXTFD);
        if (tfd & AHCI_PXTFD_STS_ERR) {
            printf("ahci: port %u: command %02x failed (PxTFD=%x)\n", ctx->port_index, command,
                   tfd);
            return false;
        }
        if (!(reg_read32(p, AHCI_PXCI) & (1U << 0))) {
            done = true;
            break;
        }
    }
    if (!done) {
        printf("ahci: port %u: command %02x timed out (PxTFD=%x PxCI=%x)\n", ctx->port_index,
               command, reg_read32(p, AHCI_PXTFD), reg_read32(p, AHCI_PXCI));
        return false;
    }
    return true;
}

/* disk_device_t's read/write take no device-context argument -- one pair of
 * thunks per fixed slot, identical in shape to drivers/usb_msd.c's
 * MSD_THUNK_PAIR macro. */
static int ahci_disk_read(int index, uint32_t lba, uint8_t *buffer);
static int ahci_disk_write(int index, uint32_t lba, const uint8_t *buffer);
#define AHCI_THUNK_PAIR(n) \
    static int ahci_read_##n(uint32_t lba, uint8_t *buf) { return ahci_disk_read(n, lba, buf); } \
    static int ahci_write_##n(uint32_t lba, const uint8_t *buf) \
    { \
        return ahci_disk_write(n, lba, buf); \
    }
AHCI_THUNK_PAIR(0)
AHCI_THUNK_PAIR(1)
AHCI_THUNK_PAIR(2)
AHCI_THUNK_PAIR(3)
AHCI_THUNK_PAIR(4)
AHCI_THUNK_PAIR(5)
AHCI_THUNK_PAIR(6)
AHCI_THUNK_PAIR(7)
static int (*const ahci_reads[AHCI_MAX_DISKS])(uint32_t, uint8_t *) = {
    ahci_read_0, ahci_read_1, ahci_read_2, ahci_read_3,
    ahci_read_4, ahci_read_5, ahci_read_6, ahci_read_7,
};
static int (*const ahci_writes[AHCI_MAX_DISKS])(uint32_t, const uint8_t *) = {
    ahci_write_0, ahci_write_1, ahci_write_2, ahci_write_3,
    ahci_write_4, ahci_write_5, ahci_write_6, ahci_write_7,
};
static const char *const ahci_names[AHCI_MAX_DISKS] = {
    "ahci0", "ahci1", "ahci2", "ahci3", "ahci4", "ahci5", "ahci6", "ahci7",
};

static int ahci_disk_read(int index, uint32_t lba, uint8_t *buffer)
{
    ahci_port_ctx_t *ctx = &ports[index];
    if (!ctx->present) {
        return -1;
    }
    return ahci_issue_command(ctx, AHCI_ATA_CMD_READ_DMA_EXT, lba, buffer, false) ? 0 : -1;
}

static int ahci_disk_write(int index, uint32_t lba, const uint8_t *buffer)
{
    ahci_port_ctx_t *ctx = &ports[index];
    if (!ctx->present) {
        return -1;
    }
    if (!ahci_issue_command(ctx, AHCI_ATA_CMD_WRITE_DMA_EXT, lba, (uint8_t *)(uintptr_t)buffer,
                             true)) {
        return -1;
    }
    /* Mirrors ata_write_sector()'s ATA_CMD_CACHE_FLUSH after every write --
     * see that function's own comment for why a persistent EXT2 root can't
     * skip this and rely on write-back caching alone. */
    return ahci_issue_command(ctx, AHCI_ATA_CMD_FLUSH_CACHE_EXT, 0, NULL, false) ? 0 : -1;
}

static const char *ahci_sig_name(uint32_t sig)
{
    switch (sig) {
    case AHCI_SIG_ATA:             return "ATA";
    case AHCI_SIG_ATAPI:           return "ATAPI";
    case AHCI_SIG_ENCLOSURE:       return "enclosure bridge";
    case AHCI_SIG_PORT_MULTIPLIER: return "port multiplier";
    default:                       return "unknown";
    }
}

void ahci_init(void)
{
    if (ctrl.present) {
        return;
    }

    pci_device_t dev;
    if (!pci_find_by_class(AHCI_PCI_CLASS_STORAGE, AHCI_PCI_SUBCLASS_SATA, AHCI_PCI_PROGIF_AHCI,
                            &dev)) {
        pci_device_t nvme;
        if (pci_find_by_class(NVME_PCI_CLASS_STORAGE, NVME_PCI_SUBCLASS_NVM, NVME_PCI_PROGIF_NVME,
                               &nvme)) {
            printf("ahci: no AHCI controller found, but an NVMe controller (%04x:%04x at "
                   "%02x:%02x.%u) is present -- this kernel has no NVMe driver, so any disk "
                   "behind it is unreachable\n",
                   nvme.vendor_id, nvme.device_id, nvme.bus, nvme.slot, nvme.func);
        } else {
            printf("ahci: no AHCI controller found\n");
        }
        return;
    }
    printf("ahci: found %04x:%04x at %02x:%02x.%u (irq %u)\n", dev.vendor_id, dev.device_id,
           dev.bus, dev.slot, dev.func, dev.interrupt_line);

    pci_enable_bus_mastering(&dev);

    phys_addr_t abar = pci_bar_address(&dev, 5);
    uint32_t bar_size = pci_bar_size(&dev, 5);
    if (!abar || !bar_size) {
        printf("ahci: BAR5 (ABAR) is not a usable memory-mapped BAR\n");
        return;
    }
    printf("ahci: ABAR at phys=%p size=%u KiB\n", (void *)(uintptr_t)abar, bar_size / 1024);
    ctrl.pci = dev;
    ctrl.mmio = map_bar(abar, bar_size);

    uint32_t cap = reg_read32(ctrl.mmio, AHCI_REG_CAP);
    uint32_t num_ports_cap = AHCI_CAP_NP(cap);
    printf("ahci: CAP=%x (ports_cap=%u cmd_slots=%u 64-bit=%u)\n", cap, num_ports_cap,
           AHCI_CAP_NCS(cap), AHCI_CAP_S64A(cap));

    /* AHCI Enable must be set before any port register is meaningful (sec
     * 3.1.2) -- some firmware leaves the HBA in legacy/IDE-emulation mode
     * until software claims it this way. */
    reg_write32(ctrl.mmio, AHCI_REG_GHC, reg_read32(ctrl.mmio, AHCI_REG_GHC) | AHCI_GHC_AE);

    uint32_t pi = reg_read32(ctrl.mmio, AHCI_REG_PI);
    printf("ahci: PI (ports implemented) = %x\n", pi);

    int disk_count = 0;
    for (uint32_t port = 0; port < 32; ++port) {
        if (!(pi & (1U << port))) {
            continue;
        }
        volatile uint8_t *p = ctrl.mmio + AHCI_PORT_REGS_BASE + port * AHCI_PORT_STRIDE;

        uint32_t ssts = reg_read32(p, AHCI_PXSSTS);
        if (AHCI_PXSSTS_DET(ssts) != AHCI_PXSSTS_DET_PRESENT) {
            printf("ahci: port %u: no device (PxSSTS=%x, DET=%u) -- skipping\n", port, ssts,
                   AHCI_PXSSTS_DET(ssts));
            continue;
        }

        uint32_t sig = reg_read32(p, AHCI_PXSIG);
        if (sig != AHCI_SIG_ATA) {
            printf("ahci: port %u: device present but not a plain ATA disk (PxSIG=%x, %s) -- "
                   "skipping\n",
                   port, sig, ahci_sig_name(sig));
            continue;
        }

        if (disk_count >= AHCI_MAX_DISKS) {
            printf("ahci: port %u: real ATA disk present, but %u AHCI disks are already "
                   "tracked (max %u) -- dropping\n",
                   port, disk_count, AHCI_MAX_DISKS);
            continue;
        }

        ahci_port_ctx_t *ctx = &ports[disk_count];
        ctx->port_index = port;
        ctx->port_regs = p;
        if (!port_start(ctx)) {
            printf("ahci: port %u: bring-up failed\n", port);
            continue;
        }

        ctx->disk.name = ahci_names[disk_count];
        ctx->disk.sector_size = 512;
        ctx->disk.present = true;
        ctx->disk.read = ahci_reads[disk_count];
        ctx->disk.write = ahci_writes[disk_count];
        ctx->present = true;
        printf("ahci: port %u: attached as %s\n", port, ctx->disk.name);
        ++disk_count;
    }

    ctrl.present = true;
    printf("ahci: %d disk(s) attached\n", disk_count);
}

disk_device_t *ahci_disk(int index)
{
    if (index < 0 || index >= AHCI_MAX_DISKS || !ports[index].present) {
        return NULL;
    }
    return &ports[index].disk;
}
