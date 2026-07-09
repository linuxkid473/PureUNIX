#ifndef PUREUNIX_RAMDISK_H
#define PUREUNIX_RAMDISK_H

#include <pureunix/disk.h>

/* Wraps an already-in-memory image (a GRUB multiboot2 module, identity-
 * mapped so its physical address is directly dereferenceable — see
 * kernel/vmm.c) as a disk_device_t, so fat16_mount()/ext2_mount() can use
 * it exactly like a real ATA disk. Two fixed slots, matching PureUNIX's
 * two boot images (fat.img on slot 0, root.img on slot 1) — see
 * kernel_main()'s use of pmm_module_get(). Returns NULL for any other
 * slot number. */
disk_device_t *ramdisk_attach(int slot, uint8_t *base, uint32_t size);

#endif
