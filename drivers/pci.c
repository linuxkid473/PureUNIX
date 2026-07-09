#include <pureunix/io.h>
#include <pureunix/pci.h>
#include <pureunix/stdio.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFCU);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, slot, func, (uint8_t)(offset & 0xFC));
    return (uint16_t)(value >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value)
{
    uint8_t aligned = (uint8_t)(offset & 0xFC);
    uint32_t shift = (offset & 2) * 8;
    uint32_t old = pci_config_read32(bus, slot, func, aligned);
    uint32_t merged = (old & ~(0xFFFFU << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, aligned, merged);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, slot, func, (uint8_t)(offset & 0xFC));
    return (uint8_t)(value >> ((offset & 3) * 8));
}

void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value)
{
    uint8_t aligned = (uint8_t)(offset & 0xFC);
    uint32_t shift = (offset & 3) * 8;
    uint32_t old = pci_config_read32(bus, slot, func, aligned);
    uint32_t merged = (old & ~(0xFFU << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, aligned, merged);
}

/* Fills *out for the given bus/slot/func. Returns false (leaving *out
 * untouched) if no device answers -- vendor_id reads back 0xFFFF on an
 * empty slot/function, the standard PCI "nothing here" sentinel. */
static bool pci_probe(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *out)
{
    uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) {
        return false;
    }
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = vendor;
    out->device_id = pci_config_read16(bus, slot, func, 0x02);
    uint32_t class_reg = pci_config_read32(bus, slot, func, 0x08);
    out->prog_if = (uint8_t)(class_reg >> 8);
    out->subclass = (uint8_t)(class_reg >> 16);
    out->class_code = (uint8_t)(class_reg >> 24);
    out->header_type = pci_config_read8(bus, slot, func, 0x0E);
    out->interrupt_line = pci_config_read8(bus, slot, func, 0x3C);
    return true;
}

static const char *pci_class_name(uint8_t class_code)
{
    switch (class_code) {
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x06: return "bridge";
    default:   return "other";
    }
}

/* Shared by pci_scan()/pci_find(): calls `visit` for every function of
 * every present device on every bus, stopping early if `visit` returns
 * true. Multi-function devices (header_type bit 7) are probed on all 8
 * functions; single-function devices only on function 0. */
static bool pci_walk(bool (*visit)(const pci_device_t *dev, void *ctx), void *ctx)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            pci_device_t dev;
            if (!pci_probe((uint8_t)bus, slot, 0, &dev)) {
                continue;
            }
            uint8_t functions = (dev.header_type & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < functions; ++func) {
                if (func != 0 && !pci_probe((uint8_t)bus, slot, func, &dev)) {
                    continue;
                }
                if (visit(&dev, ctx)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool pci_print_one(const pci_device_t *dev, void *ctx)
{
    (void)ctx;
    printf("  %02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x (%s)\n",
           dev->bus, dev->slot, dev->func, dev->vendor_id, dev->device_id,
           dev->class_code, dev->subclass, pci_class_name(dev->class_code));
    return false;
}

void pci_scan(void)
{
    printf("PCI: enumerating buses\n");
    pci_walk(pci_print_one, NULL);
}

typedef struct pci_find_ctx {
    uint16_t vendor_id;
    const uint16_t *device_ids;
    int count;
    pci_device_t *out;
} pci_find_ctx_t;

static bool pci_match_one(const pci_device_t *dev, void *ctx_ptr)
{
    pci_find_ctx_t *ctx = (pci_find_ctx_t *)ctx_ptr;
    if (dev->vendor_id != ctx->vendor_id) {
        return false;
    }
    for (int i = 0; i < ctx->count; ++i) {
        if (dev->device_id == ctx->device_ids[i]) {
            *ctx->out = *dev;
            return true;
        }
    }
    return false;
}

bool pci_find(uint16_t vendor_id, const uint16_t *device_ids, int count, pci_device_t *out)
{
    pci_find_ctx_t ctx = { vendor_id, device_ids, count, out };
    return pci_walk(pci_match_one, &ctx);
}

typedef struct pci_find_class_ctx {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    pci_device_t *out;
} pci_find_class_ctx_t;

static bool pci_match_class_one(const pci_device_t *dev, void *ctx_ptr)
{
    pci_find_class_ctx_t *ctx = (pci_find_class_ctx_t *)ctx_ptr;
    if (dev->class_code == ctx->class_code && dev->subclass == ctx->subclass
        && dev->prog_if == ctx->prog_if) {
        *ctx->out = *dev;
        return true;
    }
    return false;
}

bool pci_find_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out)
{
    pci_find_class_ctx_t ctx = { class_code, subclass, prog_if, out };
    return pci_walk(pci_match_class_one, &ctx);
}

void pci_enable_bus_mastering(const pci_device_t *dev)
{
    uint16_t command = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    command |= 0x0004U /* Bus Master Enable */ | 0x0002U /* Memory Space Enable */;
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, command);
}

void pci_enable_legacy_interrupts(const pci_device_t *dev)
{
    uint16_t command = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    command &= ~0x0400U /* Interrupt Disable, bit 10 */;
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, command);
}

/* BAR memory-type field (bits 2:1 of a memory-space BAR dword): 0b00 means
 * 32-bit, 0b10 means 64-bit (the next BAR slot holds the high dword). 0b01
 * ("reserved" pre-PCI 2.1 below-1MiB type) never appears on real hardware
 * this kernel targets and is treated the same as 32-bit. */
static bool bar_is_64bit_dword(uint32_t bar)
{
    return !(bar & 0x1U) && ((bar >> 1) & 0x3U) == 0x2U;
}

bool pci_bar_is_64bit(const pci_device_t *dev, int index)
{
    if (index < 0 || index > 5) {
        return false;
    }
    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t bar = pci_config_read32(dev->bus, dev->slot, dev->func, offset);
    return bar_is_64bit_dword(bar);
}

phys_addr_t pci_bar_address(const pci_device_t *dev, int index)
{
    if (index < 0 || index > 5) {
        return 0;
    }
    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t bar = pci_config_read32(dev->bus, dev->slot, dev->func, offset);
    if (bar & 0x1U) {
        return 0; /* I/O-space BAR, not memory-mapped */
    }
    if (bar_is_64bit_dword(bar) && index < 5) {
        uint32_t high = pci_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(offset + 4));
        if (high != 0) {
            /* Genuinely above 4GB -- this kernel is 32-bit/non-PAE and has
             * no way to map or address such a region. */
            printf("pci: BAR%d at %02x:%02x.%u is a 64-bit BAR above 4GB (high=%x); unusable\n",
                   index, dev->bus, dev->slot, dev->func, high);
            return 0;
        }
    }
    return bar & ~0xFU;
}

uint32_t pci_bar_size(const pci_device_t *dev, int index)
{
    if (index < 0 || index > 5) {
        return 0;
    }
    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t original = pci_config_read32(dev->bus, dev->slot, dev->func, offset);
    if (original & 0x1U) {
        return 0;
    }
    pci_config_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFU);
    uint32_t sized = pci_config_read32(dev->bus, dev->slot, dev->func, offset);
    pci_config_write32(dev->bus, dev->slot, dev->func, offset, original);
    uint32_t mask = sized & ~0xFU;
    if (mask == 0) {
        return 0;
    }
    return ~mask + 1U;
}
