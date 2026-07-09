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

/* Same walk as pci_scan()/pci_find(), but matches on class/subclass/prog-if
 * (config offset 0x08-0x0B) instead of vendor/device ID -- used by
 * xhci_init() to find "the xHCI controller" (class 0x0C, subclass 0x03,
 * prog-if 0x30) without needing to know its vendor/device ID in advance,
 * since that varies across chipsets and QEMU machine types. Returns false if
 * no match was found. */
bool pci_find_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);

/* Sets the Bus Master Enable and Memory Space Enable bits in the PCI command
 * register (offset 0x04) -- required before a device's own DMA engine (e.g.
 * e1000 descriptor rings) is allowed to read/write system memory. */
void pci_enable_bus_mastering(const pci_device_t *dev);

/* Clears the Interrupt Disable bit (bit 10) in the PCI command register --
 * required for legacy INTx delivery to reach the CPU at all. Unlike Bus
 * Master/Memory Space Enable, most devices don't need this cleared by
 * default (it powers up clear), but some firmware/QEMU machine types set it,
 * and a device using MSI never needs it cleared -- so this is a separate
 * call rather than folded into pci_enable_bus_mastering(). */
void pci_enable_legacy_interrupts(const pci_device_t *dev);

/* Physical base address of BAR `index` (0-5). Returns 0 if the BAR is an
 * I/O-space BAR (bit0 set) rather than memory-mapped, or if the index is
 * out of range.
 *
 * Transparently handles 64-bit BARs (memory-type bits 2:1 == 0b10, which
 * consume two consecutive 32-bit BAR slots: `index` holds the low dword,
 * `index+1` holds the high dword) by combining both dwords. Since this
 * kernel is 32-bit with no PAE and cannot address physical memory above
 * 4GB, a 64-bit BAR whose high dword is nonzero is treated as unusable:
 * this function returns 0 and logs a warning rather than truncating the
 * address. Callers that walk BARs by index (there are none in this kernel
 * today, but keep this in mind if one is added) must skip index+1 once
 * index is found to be a 64-bit BAR -- pci_bar_is_64bit() reports this. */
phys_addr_t pci_bar_address(const pci_device_t *dev, int index);

/* Size in bytes of BAR `index`, found via the standard "write all-ones,
 * read back, restore" probe. Returns 0 for I/O-space BARs or an absent BAR.
 * For a 64-bit BAR, the low dword's sizing mask alone determines the size
 * (a sub-4GB region's size never depends on the high dword), so this needs
 * no special 64-bit handling beyond what pci_bar_is_64bit() callers already
 * account for when skipping index+1. */
uint32_t pci_bar_size(const pci_device_t *dev, int index);

/* True if BAR `index` is a 64-bit (memory-type bits 2:1 == 0b10) BAR, i.e.
 * whether `index+1` is consumed as its high dword rather than being an
 * independent BAR. False for I/O-space BARs, 32-bit memory BARs, and an
 * out-of-range index. */
bool pci_bar_is_64bit(const pci_device_t *dev, int index);

#endif
