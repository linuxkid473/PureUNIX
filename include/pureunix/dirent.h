#ifndef PUREUNIX_DIRENT_H
#define PUREUNIX_DIRENT_H

#include <pureunix/config.h>
#include <pureunix/types.h>

/* Layout must match struct dirent in user/libpure.h. One entry as returned
 * by the SYS_READDIR syscall — a flat array, no cursor/offset semantics
 * (the whole directory is enumerated into the caller's buffer in one call,
 * up to the caller-supplied capacity). */
struct pureunix_dirent {
    char     name[PUREUNIX_MAX_NAME];
    uint32_t type;   /* 1 = file, 2 = directory, 3 = symlink (see vfs_node_type_t) */
    uint32_t size;
};

#endif
