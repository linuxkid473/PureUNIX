#include <pureunix/mbr.h>
#include <pureunix/string.h>

#define MBR_SIG_OFFSET   510
#define MBR_PART_TABLE_OFFSET 446
#define MBR_PART_ENTRY_SIZE   16

bool mbr_find_partition(disk_device_t *base, uint8_t type,
                         uint32_t *out_start_lba, uint32_t *out_sector_count)
{
    if (!base || !base->present || !base->read) {
        return false;
    }

    uint8_t mbr[512];
    if (base->read(0, mbr) != 0) {
        return false;
    }
    if (mbr[MBR_SIG_OFFSET] != 0x55 || mbr[MBR_SIG_OFFSET + 1] != 0xAA) {
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
