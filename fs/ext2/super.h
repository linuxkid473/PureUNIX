#ifndef EXT2_SUPER_H
#define EXT2_SUPER_H

#include "ext2.h"
#include <pureunix/disk.h>

/* Returns a pointer to the global EXT2 state.  All other ext2 modules call
   this rather than maintaining their own copy of the pointer. */
ext2_fs_t *ext2_get_fs(void);

/* Read and validate the superblock from disk; populate g_ext2_fs.
   Returns 0 on success, negative on error. */
int ext2_super_read(disk_device_t *disk);

/* Release resources held by the global state (BGDT buffer, etc.). */
void ext2_super_free(void);

#endif
