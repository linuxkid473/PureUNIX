#ifndef PUREUNIX_MBR_H
#define PUREUNIX_MBR_H

#include <pureunix/disk.h>

#define MBR_PART_TYPE_LINUX 0x83

/* Reads the MBR (LBA 0) of `base` and looks for the first partition table
 * entry (of the 4 primary slots) whose type byte matches `type`. Returns
 * false (and leaves the out-params untouched) if `base` isn't present, LBA 0
 * doesn't read, the 0x55AA boot signature is missing, or no entry matches —
 * all treated as "not a partitioned disk we understand" rather than an
 * error, since this is used as a best-effort probe over every disk the
 * kernel finds (see kernel_main()'s root search). */
bool mbr_find_partition(disk_device_t *base, uint8_t type,
                         uint32_t *out_start_lba, uint32_t *out_sector_count);

/* Wraps a sub-range [start_lba, start_lba+sector_count) of `base` as its own
 * disk_device_t, translating every read/write by adding start_lba — lets
 * ext2_mount() (or anything else) address the partition directly starting
 * at its own LBA 0, exactly like a real partition device node would. Single
 * static slot (only ever need one: the root partition) — mirrors
 * drivers/ramdisk.c and drivers/ata.c's fixed-slot, no-`self`-pointer style,
 * since disk_device_t's read/write take no device-context argument. A
 * second call overwrites the first slot's backing range. */
disk_device_t *mbr_partition_disk(disk_device_t *base, uint32_t start_lba,
                                   uint32_t sector_count);

#endif
