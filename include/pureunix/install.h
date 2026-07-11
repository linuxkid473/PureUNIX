#ifndef PUREUNIX_INSTALL_H
#define PUREUNIX_INSTALL_H

#include <pureunix/types.h>

/* Volume label the installer stamps onto the target disk's EXT2 superblock
 * (fs/ext2/mkfs.c's ext2_mkfs()) and kernel_main() searches for when no
 * GRUB ramdisk module is present (Milestone 9) -- kept in exactly one
 * place so it can never desync between the writer and the reader. */
#define PUREUNIX_ROOT_LABEL "PUREUNIX_ROOT"

/* -------------------------------------------------------------------------
 * Wire-format structs for the disk/partition/mkfs/mount syscalls (see
 * include/pureunix/syscall.h's SYS_DISK_LIST..SYS_SYNC). Fixed-width
 * fields only (no bool, no enum) so the layout is unambiguous across the
 * kernel/userspace boundary -- mirrored byte-for-byte in user/libpure.h
 * (see that file's own copy, and struct pureunix_stat in
 * include/pureunix/stat.h / user/libpure.h for this codebase's established
 * "kept in sync by convention, not by sharing one header" pattern, since
 * user/libpure.h can't include kernel-only headers like this one).
 * ---------------------------------------------------------------------- */

#define PUREUNIX_DISK_MAX_NAME 16

typedef struct pureunix_disk_info {
    char name[PUREUNIX_DISK_MAX_NAME];
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t kind;      /* 1=ATA 2=USB 3=PARTITION 4=RAM -- see disk_kind_t */
    uint32_t removable; /* 0 or 1 */
    uint32_t present;   /* 0 or 1 */
} pureunix_disk_info_t;

/* One partition table entry, for SYS_DISK_PARTITION -- mirrors
 * partition_layout_entry_t (include/pureunix/partition.h) with fixed-width
 * fields only. */
typedef struct pureunix_partition_entry {
    uint32_t type;     /* partition type byte, e.g. 0x83 */
    uint32_t bootable; /* 0 or 1 */
    uint32_t start_lba;
    uint32_t sector_count;
} pureunix_partition_entry_t;

#define PUREUNIX_MAX_PARTITION_ENTRIES 4

typedef struct pureunix_partition_req {
    char disk_name[PUREUNIX_DISK_MAX_NAME];
    /* Must equal the target disk's current sector_count (from
     * SYS_DISK_LIST) -- a second, cheap guard beyond the root-only gate
     * against a stale/wrong disk name (e.g. a disk unplugged and replugged
     * as a different device between listing and partitioning) destroying
     * the wrong disk. Rejected with -EINVAL if it doesn't match. */
    uint32_t confirm_sector_count;
    uint32_t count; /* 1..PUREUNIX_MAX_PARTITION_ENTRIES */
    pureunix_partition_entry_t entries[PUREUNIX_MAX_PARTITION_ENTRIES];
} pureunix_partition_req_t;

typedef struct pureunix_rawio_req {
    char disk_name[PUREUNIX_DISK_MAX_NAME];
    uint32_t lba;
    /* SYS_RAWWRITE only (same confirmation-token guard as
     * pureunix_partition_req_t above); ignored by SYS_RAWREAD, which is
     * non-destructive and needs no such guard. */
    uint32_t confirm_sector_count;
    void *buf; /* exactly one sector, the disk's native sector_size bytes */
} pureunix_rawio_req_t;

typedef struct pureunix_mkfs_req {
    char disk_name[PUREUNIX_DISK_MAX_NAME];
    uint32_t start_sector;
    uint32_t sector_count;
    char label[PUREUNIX_DISK_MAX_NAME];
    /* Out param: the freshly-generated 128-bit volume UUID, filled in by
     * the kernel on success -- lets the installer embed it into the
     * grub.cfg it writes for this specific disk (`pureunix.root_uuid=`),
     * so kernel_main()'s root selection is unambiguous even if another
     * PUREUNIX_ROOT-labeled disk is also attached at boot. */
    uint8_t out_uuid[16];
} pureunix_mkfs_req_t;

/* SYS_STATFS's wire result -- mirrors vfs_statfs_t (include/pureunix/vfs.h)
 * with fixed-width fields only, for BusyBox's df/mount applets via
 * newlib's statfs(). */
typedef struct pureunix_statfs {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
} pureunix_statfs_t;

#endif
