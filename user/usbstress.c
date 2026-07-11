#include "libpure.h"

/* Sustained USB Mass Storage write/readback stress test -- exercises the
 * xHCI Bulk transfer ring across MANY sequential single-sector writes,
 * forcing the ring to wrap (XHCI_TRBS_PER_PAGE=256 TRBs per ring, 255
 * usable after the mandatory Link TRB) several times over, then reads
 * every sector back and verifies content byte-for-byte. Deliberately
 * targets the same raw disk_device_t path mkfs/EXT2 use (pu_disk_write/
 * pu_disk_read), not a filesystem -- this is meant to isolate and confirm
 * the transport layer (xHCI ring + USB Mass Storage BOT) is correct on
 * its own, independent of anything ext2-specific.
 *
 * Picks the LAST disk in the list (matches user/install.c's own
 * assumption about which disk is the freshly-attached target, not the
 * boot media) -- run this against a scratch/target disk only, it
 * overwrites every LBA it touches. */

#define STRESS_SECTOR_COUNT 3000U /* several full ring wraps (255 TRBs/wrap) */

static void print_uint(unsigned int v)
{
    pu_puti((int)v);
}

int main(void)
{
    pu_disk_info_t infos[16];
    int n = pu_disk_list(infos, 16);
    if (n <= 0) {
        pu_puts("USBSTRESS: no disks found\n");
        return 1;
    }
    if (n > 16) n = 16;

    const char *name = infos[n - 1].name;
    unsigned int sector_count = infos[n - 1].sector_count;
    pu_puts("USBSTRESS: target="); pu_puts(name);
    pu_puts(" sectors="); print_uint(sector_count);
    pu_puts("\n");

    unsigned int test_sectors = STRESS_SECTOR_COUNT;
    if (test_sectors > sector_count) {
        test_sectors = sector_count;
    }
    pu_puts("USBSTRESS: writing "); print_uint(test_sectors);
    pu_puts(" sectors (forces multiple xHCI transfer-ring wraps)...\n");

    unsigned char buf[512];
    unsigned int write_failures = 0;
    for (unsigned int lba = 0; lba < test_sectors; ++lba) {
        /* Deterministic, LBA-dependent pattern -- every sector gets
         * distinguishable content so a readback mismatch pinpoints
         * exactly which LBA (and so which point in the ring's wrap
         * cycle) failed. */
        for (unsigned int i = 0; i < sizeof(buf); ++i) {
            buf[i] = (unsigned char)((lba * 37U + i) & 0xFFU);
        }
        int rc = pu_disk_write(name, lba, sector_count, buf);
        if (rc != 0) {
            pu_puts("USBSTRESS: write lba="); print_uint(lba);
            pu_puts(" FAILED rc="); print_uint((unsigned int)(-rc));
            pu_puts("\n");
            ++write_failures;
        }
        if (lba % 500U == 0U && lba != 0U) {
            pu_puts("USBSTRESS: ."); print_uint(lba); pu_puts(" sectors written\n");
        }
    }
    pu_puts("USBSTRESS: write pass done, failures="); print_uint(write_failures);
    pu_puts("\n");

    unsigned int read_failures = 0;
    unsigned int mismatch_failures = 0;
    for (unsigned int lba = 0; lba < test_sectors; ++lba) {
        unsigned char rbuf[512];
        int rc = pu_disk_read(name, lba, rbuf);
        if (rc != 0) {
            pu_puts("USBSTRESS: read lba="); print_uint(lba);
            pu_puts(" FAILED rc="); print_uint((unsigned int)(-rc));
            pu_puts("\n");
            ++read_failures;
            continue;
        }
        int mismatch = 0;
        for (unsigned int i = 0; i < sizeof(rbuf); ++i) {
            unsigned char expected = (unsigned char)((lba * 37U + i) & 0xFFU);
            if (rbuf[i] != expected) {
                mismatch = 1;
                break;
            }
        }
        if (mismatch) {
            pu_puts("USBSTRESS: MISMATCH at lba="); print_uint(lba); pu_puts("\n");
            ++mismatch_failures;
        }
        if (lba % 500U == 0U && lba != 0U) {
            pu_puts("USBSTRESS: ."); print_uint(lba); pu_puts(" sectors verified\n");
        }
    }

    pu_puts("USBSTRESS: done. sectors="); print_uint(test_sectors);
    pu_puts(" write_failures="); print_uint(write_failures);
    pu_puts(" read_failures="); print_uint(read_failures);
    pu_puts(" mismatches="); print_uint(mismatch_failures);
    pu_puts("\n");

    if (write_failures == 0U && read_failures == 0U && mismatch_failures == 0U) {
        pu_puts("USBSTRESS: PASS\n");
        return 0;
    }
    pu_puts("USBSTRESS: FAIL\n");
    return 1;
}
