#ifndef PUREUNIX_PARTITION_H
#define PUREUNIX_PARTITION_H

#include <pureunix/disk.h>

/* MBR partitioning (fs/partition.c). GPT is out of scope for now (see
 * docs/install.md) -- every disk this kernel creates or reads uses a
 * classic 4-primary-entry MBR. */

#define PARTITION_TYPE_LINUX 0x83U

typedef struct partition_layout_entry {
    uint8_t type;         /* partition type byte, e.g. PARTITION_TYPE_LINUX */
    bool bootable;
    uint32_t start_lba;
    uint32_t sector_count;
} partition_layout_entry_t;

/* Scans every registered whole disk (kind != DISK_KIND_PARTITION) for a
 * valid MBR (0x55AA signature at byte 510) and registers each non-empty
 * primary partition entry found as its own disk_device_t, named
 * "<parent-name><1-based-index>" (e.g. "sda" -> "sda1"/"sda2"/...), whose
 * read/write translate into the parent disk's own read/write shifted by
 * the partition's start LBA. A disk with no valid MBR (unpartitioned, or a
 * foreign/unrecognized layout) is silently skipped -- not an error. Safe
 * to call more than once: a disk whose partitions have already been
 * registered is left alone, so re-scanning after a new disk attaches (a
 * freshly plugged USB drive) only adds what's new. */
void disk_scan_partitions(void);

/* Builds and writes a complete, valid MBR (a zeroed bootstrap area,
 * `entries[0..count)` packed into the four primary partition slots, and
 * the 0x55AA boot signature) to LBA 0 of `disk`. This is the one place
 * that ever hand-assembles an MBR's bytes -- both the installer (via
 * SYS_DISK_PARTITION) and any future partitioning tool go through this
 * rather than re-implementing the byte layout. The bootstrap area is left
 * zeroed deliberately: installing a real bootloader (GRUB's boot.img) onto
 * LBA 0 is a separate, later step (see docs/install.md) that splices its
 * own bootstrap code over bytes 0-445 while leaving the partition table
 * this function just wrote (bytes 446-509) and the signature untouched.
 *
 * Every entry is validated (nonzero start/count, fits within
 * disk->sector_count when known, no two entries overlap) *before* writing
 * anything -- on any validation failure the disk is left completely
 * untouched and a negative errno is returned. `count` must be 1..4 (MBR
 * has exactly four primary slots; extended/logical partitions are out of
 * scope). Returns 0 on success. */
int partition_write_mbr(disk_device_t *disk, const partition_layout_entry_t *entries, int count);

#endif
