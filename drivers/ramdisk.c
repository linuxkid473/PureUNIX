#include <pureunix/ramdisk.h>
#include <pureunix/string.h>

#define RAMDISK_SLOTS 2
#define RAMDISK_SECTOR_SIZE 512

typedef struct {
    uint8_t *base;
    uint32_t sector_count;
} ramdisk_backing_t;

static ramdisk_backing_t backing[RAMDISK_SLOTS];

static int ramdisk_read(int slot, uint32_t lba, uint8_t *buffer)
{
    if (!backing[slot].base || lba >= backing[slot].sector_count) {
        return -1;
    }
    memcpy(buffer, backing[slot].base + (uint32_t)lba * RAMDISK_SECTOR_SIZE, RAMDISK_SECTOR_SIZE);
    return 0;
}

static int ramdisk_write(int slot, uint32_t lba, const uint8_t *buffer)
{
    if (!backing[slot].base || lba >= backing[slot].sector_count) {
        return -1;
    }
    memcpy(backing[slot].base + (uint32_t)lba * RAMDISK_SECTOR_SIZE, buffer, RAMDISK_SECTOR_SIZE);
    return 0;
}

/* disk_device_t's read/write take no device-context argument (see
 * drivers/ata.c's identical master/slave split), so each slot needs its
 * own pair of thunks. */
static int ramdisk_read_0(uint32_t lba, uint8_t *buffer) { return ramdisk_read(0, lba, buffer); }
static int ramdisk_write_0(uint32_t lba, const uint8_t *buffer) { return ramdisk_write(0, lba, buffer); }
static int ramdisk_read_1(uint32_t lba, uint8_t *buffer) { return ramdisk_read(1, lba, buffer); }
static int ramdisk_write_1(uint32_t lba, const uint8_t *buffer) { return ramdisk_write(1, lba, buffer); }

static disk_device_t ramdisks[RAMDISK_SLOTS] = {
    { .name = "ram0", .sector_size = RAMDISK_SECTOR_SIZE, .present = false,
      .read = ramdisk_read_0, .write = ramdisk_write_0 },
    { .name = "ram1", .sector_size = RAMDISK_SECTOR_SIZE, .present = false,
      .read = ramdisk_read_1, .write = ramdisk_write_1 },
};

disk_device_t *ramdisk_attach(int slot, uint8_t *base, uint32_t size)
{
    if (slot < 0 || slot >= RAMDISK_SLOTS) {
        return NULL;
    }
    backing[slot].base = base;
    backing[slot].sector_count = size / RAMDISK_SECTOR_SIZE;
    ramdisks[slot].present = (base != NULL && size >= RAMDISK_SECTOR_SIZE);
    return &ramdisks[slot];
}
