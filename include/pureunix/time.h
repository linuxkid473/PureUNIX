#ifndef PUREUNIX_TIME_H
#define PUREUNIX_TIME_H

#include <pureunix/types.h>

/* Current wall-clock time as a Unix epoch second count, read live from the
 * CMOS real-time clock on every call (there is no ticking in-kernel clock
 * to advance separately). Used by the EXT2 write path to stamp real
 * atime/mtime/ctime on writable filesystem operations — the same CMOS
 * registers the shell's `date` builtin already reads, just converted the
 * opposite direction (civil date -> epoch, rather than epoch -> civil). */
uint32_t time_now(void);

#endif
