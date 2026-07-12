/*
htop - pureunix/Platform.c
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "pureunix/Platform.h"

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "CPUMeter.h"
#include "DateTimeMeter.h"
#include "FileDescriptorMeter.h"
#include "HostnameMeter.h"
#include "LoadAverageMeter.h"
#include "Macros.h"
#include "MemoryMeter.h"
#include "MemorySwapMeter.h"
#include "SwapMeter.h"
#include "SysArchMeter.h"
#include "TasksMeter.h"
#include "UptimeMeter.h"
#include "pureunix/PureUnixMachine.h"


const ScreenDefaults Platform_defaultScreens[] = {
   {
      .name = "Main",
      .columns = "PID USER PRIORITY NICE M_VIRT M_RESIDENT STATE PERCENT_CPU PERCENT_MEM TIME Command",
      .sortKey = "PERCENT_CPU",
   },
};

const unsigned int Platform_numberOfDefaultScreens = ARRAYSIZE(Platform_defaultScreens);

/* Real signal numbers, matching third_party/newlib's own <sys/signal.h>
 * BSD numbering exactly (see project docs/process-management.md and the
 * ncurses port's own SIGWINCH addition) — every one of these reaches a
 * real task_kill()/signal_send() (kernel/signal.c), not a subset that
 * merely "looks like" a signal list. F9 in htop sends whichever of these
 * is selected via the real SYS_KILL syscall. */
const SignalItem Platform_signals[] = {
   { .name = " 0 Cancel",   .number =  0 },
   { .name = " 1 SIGHUP",   .number =  1 },
   { .name = " 2 SIGINT",   .number =  2 },
   { .name = " 3 SIGQUIT",  .number =  3 },
   { .name = " 4 SIGILL",   .number =  4 },
   { .name = " 6 SIGABRT",  .number =  6 },
   { .name = " 8 SIGFPE",   .number =  8 },
   { .name = " 9 SIGKILL",  .number =  9 },
   { .name = "10 SIGBUS",   .number = 10 },
   { .name = "11 SIGSEGV",  .number = 11 },
   { .name = "13 SIGPIPE",  .number = 13 },
   { .name = "14 SIGALRM",  .number = 14 },
   { .name = "15 SIGTERM",  .number = 15 },
   { .name = "17 SIGSTOP",  .number = 17 },
   { .name = "18 SIGTSTP",  .number = 18 },
   { .name = "19 SIGCONT",  .number = 19 },
   { .name = "20 SIGCHLD",  .number = 20 },
   { .name = "28 SIGWINCH", .number = 28 },
   { .name = "30 SIGUSR1",  .number = 30 },
   { .name = "31 SIGUSR2",  .number = 31 },
};

const unsigned int Platform_numberOfSignals = ARRAYSIZE(Platform_signals);

enum {
   MEMORY_CLASS_USED = 0,
   MEMORY_CLASS_CACHED,
};

const MemoryClass Platform_memoryClasses[] = {
   [MEMORY_CLASS_USED] = { .label = "used", .countsAsUsed = true, .countsAsCache = false, .color = MEMORY_1 },
   [MEMORY_CLASS_CACHED] = { .label = "cached", .countsAsUsed = false, .countsAsCache = true, .color = MEMORY_2 },
};

const unsigned int Platform_numberOfMemoryClasses = ARRAYSIZE(Platform_memoryClasses);

const MeterClass* const Platform_meterTypes[] = {
   &CPUMeter_class,
   &ClockMeter_class,
   &DateMeter_class,
   &DateTimeMeter_class,
   &LoadAverageMeter_class,
   &LoadMeter_class,
   &MemoryMeter_class,
   &SwapMeter_class,
   &MemorySwapMeter_class,
   &TasksMeter_class,
   &HostnameMeter_class,
   &SysArchMeter_class,
   &UptimeMeter_class,
   &SecondsUptimeMeter_class,
   &BlankMeter_class,
   NULL
};

static const char PureUnix_release[] = "PureUNIX";

bool Platform_init(void) {
   return true;
}

void Platform_done(void) {
}

void Platform_setBindings(Htop_Action* keys) {
   (void) keys;
}

/* /proc/uptime: "%u.%02u 0.00\n" — fs/procfs.c's render_proc_uptime(),
 * this kernel's real PIT tick count since boot. */
int Platform_getUptime(void) {
   FILE* f = fopen(PROCDIR "/uptime", "r");
   if (!f) {
      return 0;
   }
   double uptime = 0.0;
   int matched = fscanf(f, "%lf", &uptime);
   fclose(f);
   return matched == 1 ? (int) uptime : 0;
}

/* No load-average tracking exists in this kernel (no exponentially
 * decayed run-queue-length sampler) — reporting 0/0/0 honestly, the same
 * choice the unsupported/ reference platform makes, rather than
 * fabricating numbers nothing backs. */
void Platform_getLoadAverage(double* one, double* five, double* fifteen) {
   *one = 0;
   *five = 0;
   *fifteen = 0;
}

pid_t Platform_getMaxPid(void) {
   /* task_t.id is a plain uint32_t next_task_id counter (kernel/task.c) —
    * no small fixed ceiling the way Linux's pid_max is, but a real 32-bit
    * pid_t range is a safe, honest upper bound for column-width sizing. */
   return INT_MAX;
}

double Platform_setCPUValues(Meter* this, unsigned int cpu) {
   (void) cpu;
   const PureUnixMachine* phost = (const PureUnixMachine*) this->host;

   double* v = this->values;
   v[CPU_METER_NORMAL] = phost->cpuPercent;
   v[CPU_METER_FREQUENCY] = NAN;
   v[CPU_METER_TEMPERATURE] = NAN;
   this->curItems = 1;

   return phost->cpuPercent;
}

void Platform_setMemoryValues(Meter* this) {
   const Machine* host = this->host;
   const PureUnixMachine* phost = (const PureUnixMachine*) host;

   double* v = this->values;
   v[MEMORY_CLASS_USED] = (double) phost->usedMem;
   v[MEMORY_CLASS_CACHED] = NAN;
   this->curItems = 1;
   this->total = (double) host->totalMem;
}

void Platform_setSwapValues(Meter* this) {
   /* No swap exists on this kernel (fs/procfs.c's render_proc_meminfo()
    * has no SwapTotal/SwapFree line at all — there's no disk-backed
    * paging to report on), so this stays a real, honest zero. */
   this->total = 0;
   this->curItems = 0;
}

char* Platform_getProcessEnv(pid_t pid) {
   /* No /proc/[pid]/environ exists yet (fs/procfs.c) — NULL (rather than
    * a fabricated environment) is exactly what htop's EnvScreen expects
    * for "not available". */
   (void) pid;
   return NULL;
}

FileLocks_ProcessData* Platform_getProcessLocks(pid_t pid) {
   (void) pid;
   return NULL;
}

void Platform_getFileDescriptors(double* used, double* max) {
   /* No global open-file-descriptor accounting exists (MAX_OPEN_FILES,
    * include/pureunix/task.h, is a fixed per-task limit, not a system-
    * wide counter) — NAN is htop's own "not available" convention for a
    * meter value, more honest than a made-up number. */
   *used = NAN;
   *max = NAN;
}

bool Platform_getDiskIO(DiskIOData* data) {
   (void) data;
   return false;
}

bool Platform_getNetworkIO(NetworkIOData* data) {
   (void) data;
   return false;
}

void Platform_getBattery(double* percent, ACPresence* isOnAC) {
   *percent = NAN;
   *isOnAC = AC_ERROR;
}

void Platform_getHostname(char* buffer, size_t size) {
   if (gethostname(buffer, size) != 0) {
      String_safeStrncpy(buffer, PureUnix_release, size);
   }
}

const char* Platform_getRelease(void) {
   return PureUnix_release;
}
