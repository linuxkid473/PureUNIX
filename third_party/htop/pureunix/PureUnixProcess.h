#ifndef HEADER_PureUnixProcess
#define HEADER_PureUnixProcess
/*
htop - pureunix/PureUnixProcess.h
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include <stdbool.h>

#include "Machine.h"
#include "Process.h"


typedef struct PureUnixProcess_ {
   Process super;
   /* Previous scan's CPU-tick count (fs/procfs.c's /proc/[pid]/stat utime
    * field, this kernel's own 100 Hz PIT ticks) — kept here (not
    * recomputed from nothing) so percent_cpu can be a real delta over
    * elapsed wall time between two scans, the same technique every real
    * htop backend uses. 0 until this process has been seen twice. */
   unsigned long long prevUtime;
} PureUnixProcess;

extern const ProcessClass PureUnixProcess_class;

Process* PureUnixProcess_new(const Machine* host);

#endif
