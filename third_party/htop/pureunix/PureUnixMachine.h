#ifndef HEADER_PureUnixMachine
#define HEADER_PureUnixMachine
/*
htop - pureunix/PureUnixMachine.h
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include <stdint.h>

#include "Machine.h"


typedef struct PureUnixMachine_ {
   Machine super;

   /* Previous scan's /proc/stat "cpu" line snapshot (fs/procfs.c's real
    * PIT-tick counters), for a real delta-based aggregate CPU% — the
    * same technique PureUnixProcess.h's prevUtime uses per-process. Both
    * start at 0, meaning the very first scan after startup reports 0%
    * (correct: there's no previous sample to diff against yet). */
   uint32_t prevCPUUsed;
   uint32_t prevCPUTotal;
   double cpuPercent;

   /* Real /proc/meminfo snapshot for Platform_setMemoryValues(). */
   memory_t usedMem;
} PureUnixMachine;

#endif
