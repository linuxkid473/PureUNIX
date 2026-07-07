#ifndef PUREUNIX_PCI_H
#define PUREUNIX_PCI_H

#include <pureunix/types.h>

typedef struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint8_t  interrupt_line;
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

/* Walks every bus/slot/function, printing one line per device found (vendor,
 * device, class/subclass) -- the visible "PCI bus enumeration" step. */
void pci_scan(void);

/* Same walk as pci_scan(), but stops at the first device whose vendor_id
 * matches and whose device_id is one of device_ids[0..count). Used by
 * e1000_init() to locate the NIC without hardcoding a bus/slot/func, since
 * that varies between QEMU/real hardware and even between QEMU machine
 * configurations. Returns false if no match was found. */
bool pci_find(uint16_t vendor_id, const uint16_t *device_ids, int count, pci_device_t *out);

/* Sets the Bus Master Enable and Memory Space Enable bits in the PCI command
 * register (offset 0x04) -- required before a device's own DMA engine (e.g.
 * e1000 descriptor rings) is allowed to read/write system memory. */
void pci_enable_bus_mastering(const pci_device_t *dev);

/* Physical base address of BAR `index` (0-5). Returns 0 if the BAR is an
 * I/O-space BAR (bit0 set) rather than memory-mapped, or if the index is
 * out of range. */
phys_addr_t pci_bar_address(const pci_device_t *dev, int index);

/* Size in bytes of BAR `index`, found via the standard "write all-ones,
 * read back, restore" probe. Returns 0 for I/O-space BARs or an absent BAR. */
uint32_t pci_bar_size(const pci_device_t *dev, int index);

#endif
