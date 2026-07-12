/*
htop - pureunix/PureUnixMachine.c
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.

Reads PureUNIX's real /proc (fs/procfs.c) — not faked Linux files: this
kernel's procfs is a genuine, kernel-backed pseudo-filesystem that
happens to use a Linux-compatible field layout (see docs/procfs.md and
docs/htop-port.md) because BusyBox's ps/top already depend on that
format. Global (machine-wide) state only; per-process scanning is
PureUnixProcessTable.c.
*/

#include "config.h" // IWYU pragma: keep

#include "pureunix/PureUnixMachine.h"

#include <stdio.h>
#include <string.h>

#include "CRT.h"


Machine* Machine_new(UsersTable* usersTable, uid_t userId) {
   PureUnixMachine* this = xCalloc(1, sizeof(PureUnixMachine));
   Machine* super = &this->super;

   Machine_init(super, usersTable, userId);

   /* This kernel is uniprocessor (arch/i386's boot path never brings up
    * a second CPU) — one real, honestly-reported CPU, not a guess. */
   super->activeCPUs = 1;
   super->existingCPUs = 1;

   return super;
}

void Machine_delete(Machine* super) {
   Machine_done(super);
   free(super);
}

bool Machine_isCPUonline(const Machine* host, unsigned int id) {
   (void) host;
   return id == 0;
}

int Machine_getCPUPhysicalCoreID(const Machine* host, unsigned int id) {
   (void) host; (void) id;
   return 0;
}

int Machine_getCPUThreadIndex(const Machine* host, unsigned int id) {
   (void) host; (void) id;
   return 0;
}

/* /proc/stat's one line: "cpu  <used> 0 0 <idle> 0 0 0 0 0 0\n" — see
 * fs/procfs.c's render_proc_stat(). Only the two real fields matter. */
static void scanCPUTime(PureUnixMachine* this) {
   FILE* f = fopen(PROCDIR "/stat", "r");
   if (!f) {
      this->cpuPercent = 0.0;
      return;
   }

   uint32_t used = 0, idle = 0;
   char label[8];
   int matched = fscanf(f, "%7s %u %*u %*u %u", label, &used, &idle);
   fclose(f);
   if (matched != 3) {
      this->cpuPercent = 0.0;
      return;
   }

   uint32_t total = used + idle;
   if (this->prevCPUTotal != 0 && total > this->prevCPUTotal) {
      uint32_t dUsed = used - this->prevCPUUsed;
      uint32_t dTotal = total - this->prevCPUTotal;
      this->cpuPercent = dTotal ? (100.0 * (double) dUsed / (double) dTotal) : 0.0;
      if (this->cpuPercent > 100.0) {
         this->cpuPercent = 100.0;
      }
   } else {
      /* First scan since boot — no previous sample to diff against. */
      this->cpuPercent = 0.0;
   }
   this->prevCPUUsed = used;
   this->prevCPUTotal = total;
}

/* /proc/meminfo: "MemTotal:%12u kB\nMemFree:%13u kB\n" — see
 * fs/procfs.c's render_proc_meminfo(). Real kernel/pmm.c frame counts,
 * not an estimate. No swap exists on this kernel (no disk-backed paging
 * at all), so totalSwap/usedSwap/cachedSwap stay 0 (Machine_init's
 * xCalloc already zeroed them) — a real, honest "no swap" rather than a
 * faked value. */
static void scanMemoryInfo(PureUnixMachine* this) {
   Machine* super = &this->super;
   FILE* f = fopen(PROCDIR "/meminfo", "r");
   if (!f) {
      super->totalMem = 0;
      this->usedMem = 0;
      return;
   }

   unsigned long totalKb = 0, freeKb = 0;
   if (fscanf(f, "MemTotal: %lu kB MemFree: %lu kB", &totalKb, &freeKb) != 2) {
      totalKb = 0;
      freeKb = 0;
   }
   fclose(f);

   super->totalMem = (memory_t) totalKb;
   this->usedMem = totalKb > freeKb ? (memory_t) (totalKb - freeKb) : 0;
}

void Machine_scan(Machine* super) {
   PureUnixMachine* this = (PureUnixMachine*) super;
   scanMemoryInfo(this);
   scanCPUTime(this);
}
