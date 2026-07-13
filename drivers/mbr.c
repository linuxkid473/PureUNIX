#include <pureunix/mbr.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define MBR_SIG_OFFSET   510
#define MBR_PART_TABLE_OFFSET 446
#define MBR_PART_ENTRY_SIZE   16

/* Every early-return below prints exactly why -- kernel/main.c's
 * find_persistent_root_disk() tries several disks in a row, and on real
 * hardware "no persistent root found" with zero explanation is exactly the
 * kind of silent failure that leaves someone staring at the emergency
 * shell with no idea whether the USB stick was even seen, let alone why
 * its MBR didn't check out. */
bool mbr_find_partition(disk_device_t *base, uint8_t type,
                         uint32_t *out_start_lba, uint32_t *out_sector_count)
{
    if (!base || !base->present || !base->read) {
        return false; /* not a real, present disk -- not worth logging */
    }

    uint8_t mbr[512];
    if (base->read(0, mbr) != 0) {
        printf("mbr: %s: failed to read sector 0 (LBA 0) -- transport error\n", base->name);
        return false;
    }
    if (mbr[MBR_SIG_OFFSET] != 0x55 || mbr[MBR_SIG_OFFSET + 1] != 0xAA) {
        printf("mbr: %s: no valid MBR signature (0x55AA) at bytes 510-511 -- got %02x%02x\n",
               base->name, mbr[MBR_SIG_OFFSET], mbr[MBR_SIG_OFFSET + 1]);
        return false;
    }

    for (int i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + MBR_PART_TABLE_OFFSET + i * MBR_PART_ENTRY_SIZE;
        uint8_t part_type = entry[4];
        if (part_type != type) {
            continue;
        }

        uint32_t start_lba, sector_count;
        memcpy(&start_lba, entry + 8, sizeof(uint32_t));
        memcpy(&sector_count, entry + 12, sizeof(uint32_t));
        if (sector_count == 0) {
            continue;
        }

        *out_start_lba = start_lba;
        *out_sector_count = sector_count;
        return true;
    }

    printf("mbr: %s: valid MBR but no partition of type 0x%02x found\n", base->name, type);
    return false;
}

/* ---- offset-wrapping disk_device_t (single static slot) ---- */

static disk_device_t *wrap_base;
static uint32_t wrap_start_lba;
static uint32_t wrap_sector_count;

static int mbr_part_read(uint32_t lba, uint8_t *buffer)
{
    if (!wrap_base || lba >= wrap_sector_count) {
        return -1;
    }
    return wrap_base->read(wrap_start_lba + lba, buffer);
}

static int mbr_part_write(uint32_t lba, const uint8_t *buffer)
{
    if (!wrap_base || lba >= wrap_sector_count) {
        return -1;
    }
    return wrap_base->write(wrap_start_lba + lba, buffer);
}

static disk_device_t partition_disk = {
    .name = "root-part", .sector_size = 512, .present = false,
    .read = mbr_part_read, .write = mbr_part_write,
};

disk_device_t *mbr_partition_disk(disk_device_t *base, uint32_t start_lba,
                                   uint32_t sector_count)
{
    wrap_base = base;
    wrap_start_lba = start_lba;
    wrap_sector_count = sector_count;
    partition_disk.present = (base != NULL);
    return &partition_disk;
}
