#ifndef PUREUNIX_USB_MSD_H
#define PUREUNIX_USB_MSD_H

#include <pureunix/disk.h>
#include <pureunix/usb.h>

/* Fixed number of simultaneously attached USB Mass Storage devices this
 * driver tracks -- matches drivers/ramdisk.c's fixed-slot style (no dynamic
 * disk registry exists in this kernel; see kernel/main.c's
 * find_persistent_root_disk(), the only current consumer). Comfortably
 * covers "one flash drive" (the actual boot-media scenario this exists
 * for) with headroom for a couple more attached at once. */
#define USB_MSD_MAX_DEVICES 4

/* usb_msd_try_attach()-shaped hook, called from xhci_enumerate() right
 * after hid_try_attach() (see drivers/xhci.c) for every enumerated device --
 * a silent no-op (returns false) for anything that isn't a Bulk-Only
 * Transport / SCSI Mass Storage device (checked internally via
 * dev->has_bulk_endpoints, already gated on interface class 0x08 by
 * drivers/usb.c's parse_configuration()). On success, brings the device up
 * far enough to read its capacity and registers it as a disk_device_t
 * retrievable via usb_msd_disk() -- kernel/main.c's persistent-root search
 * is the intended consumer, not a general block-device API (no partition
 * scanning, no dynamic naming). Single LUN only (LUN 0) -- GET_MAX_LUN is
 * not sent; a real single-LUN flash drive (the near-universal case, and the
 * only scenario this exists to support) is unaffected either way. */
bool usb_msd_try_attach(const usb_hc_ops_t *hc, const usb_device_t *dev);

/* Returns the disk_device_t for the index'th attached, ready USB Mass
 * Storage device (0-based, in attach order), or NULL if no device occupies
 * that slot. */
disk_device_t *usb_msd_disk(int index);

#endif
