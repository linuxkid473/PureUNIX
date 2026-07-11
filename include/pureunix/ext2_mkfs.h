#ifndef PUREUNIX_EXT2_MKFS_H
#define PUREUNIX_EXT2_MKFS_H

#include <pureunix/disk.h>

/* Formats [start_sector, start_sector+sector_count) of `disk` as a fresh
 * EXT2 filesystem (1024-byte blocks, matching this driver's only supported
 * block size -- see fs/ext2/super.c's read-side validation), with volume
 * label `label` (truncated to 15 bytes + NUL, matching s_volume_name --
 * see PUREUNIX_ROOT_LABEL in include/pureunix/install.h for the label the
 * installer and kernel_main()'s root search both use).
 *
 * Unlike tools/mkext2.py (which builds an entire image in host RAM before
 * one write -- fine for an 8MB test image, infeasible for a multi-gigabyte
 * real disk inside a kernel with a small fixed heap), this writes
 * incrementally: only the metadata blocks (superblock + BGDT + bitmaps +
 * inode table, per block group) and the root directory's own data block
 * are ever touched -- the bulk data area is left as whatever was already
 * on disk, exactly like a real mkfs.ext2 (a block only becomes visible
 * once a file's inode is created and points at it with freshly-written
 * content).
 *
 * Multiple block groups are created as needed (unlike mkext2.py's fixed
 * single-group assumption), with full, non-sparse superblock+BGDT backups
 * in every group -- simpler and more conservatively correct than
 * implementing the sparse_super feature bit's partial-backup placement
 * rules. If sector_count isn't an exact multiple of one block group's
 * worth of blocks, the remainder (under 8192 blocks / 8MB) is left unused
 * rather than handled as a partial trailing group -- a deliberate
 * simplification avoiding a whole class of edge-case bugs around marking
 * a partial group's out-of-range bits used in its own bitmap, at a
 * bounded, negligible space cost on any realistically-sized target.
 *
 * out_uuid (if non-NULL) receives the fresh, randomly-generated 128-bit
 * volume UUID (crypto_random_bytes()-backed) written into the on-disk
 * superblock's s_uuid -- the stable, collision-resistant identifier
 * kernel_main()'s root-disk selection keys on instead of the (copyable,
 * collidable) PUREUNIX_ROOT label alone. The installer reports this back
 * to the caller so it can embed it into the grub.cfg it writes
 * (`pureunix.root_uuid=...`), making root selection on that specific
 * installed disk fully deterministic even if another PUREUNIX_ROOT-
 * labeled disk is also attached at boot.
 *
 * Returns 0 on success, or a negative errno (-EINVAL for a target too
 * small to hold even one block group, -EIO on any write failure -- the
 * disk may be left partially formatted in that case, matching every real
 * mkfs tool's behavior on a mid-format I/O error). */
int ext2_mkfs(disk_device_t *disk, uint32_t start_sector, uint32_t sector_count,
              const char *label, uint8_t out_uuid[16]);

#endif
