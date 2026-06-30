#ifndef PUREUNIX_DISK_H
#define PUREUNIX_DISK_H

#include <pureunix/types.h>

typedef struct disk_device {
    const char *name;
    uint32_t sector_size;
    bool present;
    int (*read)(uint32_t lba, uint8_t *buffer);
    int (*write)(uint32_t lba, const uint8_t *buffer);
} disk_device_t;

void ata_init(void);
disk_device_t *ata_primary_master(void);
int ata_read_sector(uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint32_t lba, const uint8_t *buffer);

#endif
