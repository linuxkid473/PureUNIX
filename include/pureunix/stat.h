#ifndef PUREUNIX_STAT_H
#define PUREUNIX_STAT_H

#include <pureunix/types.h>

/* Layout must match struct stat in user/libpure.h. */
struct pureunix_stat {
    uint32_t st_size;   /* file size in bytes; 0 for directories */
    uint32_t st_type;   /* 1 = file, 2 = directory */
    uint16_t st_attr;   /* FAT attribute byte */
};

#endif
