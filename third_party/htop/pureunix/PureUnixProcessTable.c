/*
htop - pureunix/PureUnixProcessTable.c
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.

The real thing: opens PureUNIX's real /proc (fs/procfs.c), lists every
live PID under it, and parses each one's /proc/[pid]/stat, /proc/[pid]/
cmdline, and /proc/[pid]/status — the same three files BusyBox's own ps/
top already depend on, so this is the third independent consumer proving
that format is real and correct, not a demo. See docs/htop-port.md.
*/

#include "config.h" // IWYU pragma: keep

#include "pureunix/PureUnixProcessTable.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CRT.h"
#include "Object.h"
#include "Process.h"
#include "Settings.h"
#include "pureunix/PureUnixMachine.h"
#include "pureunix/PureUnixProcess.h"


ProcessTable* ProcessTable_new(Machine* host, Hashtable* pidMatchList) {
   PureUnixProcessTable* this = xCalloc(1, sizeof(PureUnixProcessTable));
   Object_setClass(this, Class(ProcessTable));

   ProcessTable* super = &this->super;
   ProcessTable_init(super, Class(PureUnixProcess), host, pidMatchList);

   return super;
}

void ProcessTable_delete(Object* cast) {
   PureUnixProcessTable* this = (PureUnixProcessTable*) cast;
   ProcessTable_done(&this->super);
   free(this);
}

static ProcessState stateFromChar(char c) {
   switch (c) {
   case 'R': return RUNNING;
   case 'S': return SLEEPING;
   case 'T': return STOPPED;
   case 'Z': return ZOMBIE;
   default:  return UNKNOWN;
   }
}

typedef struct {
   pid_t pid;
   char comm[32];
   char state;
   unsigned int ppid;
   unsigned int pgrp;
   unsigned int session;
   unsigned long long utime;
   long priority;
   int nice;
   unsigned long starttime;
   unsigned long vsize;
   unsigned long rss;
} PureUnixStat;

/* Parses one /proc/[pid]/stat line — fs/procfs.c's render_pid_stat()'s
 * exact format (real Linux /proc/[pid]/stat fields 1-24; everything
 * this kernel doesn't track is a literal "0" there, skipped here via
 * %*u). comm is bracketed in parens and located via the *last* ')'
 * rather than a naive token split, matching how every real /proc/stat
 * parser (including Linux's own kernel one) has to handle a command
 * name that could itself contain spaces or parens. */
static bool parseStat(const char* line, PureUnixStat* out) {
   const char* openParen = strchr(line, '(');
   const char* closeParen = strrchr(line, ')');
   if (!openParen || !closeParen || closeParen < openParen) {
      return false;
   }
   size_t commLen = (size_t) (closeParen - openParen - 1);
   if (commLen >= sizeof(out->comm)) {
      commLen = sizeof(out->comm) - 1;
   }
   memcpy(out->comm, openParen + 1, commLen);
   out->comm[commLen] = '\0';

   out->pid = (pid_t) atoi(line);

   int n = sscanf(closeParen + 1,
      " %c %u %u %u %*u %*u %*u %*u %*u %*u %*u %llu %*u %*u %*u %ld %d %*u %*u %lu %lu %lu",
      &out->state, &out->ppid, &out->pgrp, &out->session,
      &out->utime, &out->priority, &out->nice,
      &out->starttime, &out->vsize, &out->rss);
   return n == 10;
}

/* /proc/[pid]/cmdline: NUL-separated argv, exactly like Linux — read raw
 * (it isn't NUL-terminated text) and rendered space-joined for display,
 * the same substitution every `ps`/htop does for a Linux-style cmdline. */
static void readCmdline(pid_t pid, char* out, size_t outSize) {
   char path[32];
   snprintf(path, sizeof(path), PROCDIR "/%d/cmdline", (int) pid);
   FILE* f = fopen(path, "r");
   out[0] = '\0';
   if (!f) {
      return;
   }
   size_t n = fread(out, 1, outSize - 1, f);
   fclose(f);
   out[n] = '\0';
   for (size_t i = 0; i + 1 < n; i++) {
      if (out[i] == '\0') {
         out[i] = ' ';
      }
   }
   /* Trim a trailing NUL/space left by the last argument's own
    * terminator so Process_updateCmdline() never sees an empty-looking
    * but non-empty-length string. */
   while (n > 0 && (out[n - 1] == '\0' || out[n - 1] == ' ')) {
      out[--n] = '\0';
   }
}

/* /proc/[pid]/status's Uid:/Gid: lines — see fs/procfs.c's
 * render_pid_status(). Literal-text fscanf() match against the tab/
 * newline-formatted fields it's known to always emit, in order. */
static void readIds(pid_t pid, uid_t* uid, gid_t* gid) {
   char path[32];
   snprintf(path, sizeof(path), PROCDIR "/%d/status", (int) pid);
   FILE* f = fopen(path, "r");
   *uid = 0;
   *gid = 0;
   if (!f) {
      return;
   }
   unsigned int u = 0, g = 0;
   char nameBuf[32];
   char stateBuf;
   unsigned int dummy;
   /* Name:\tX\nState:\tX\nPid:\tX\nPPid:\tX\nPGid:\tX\nSId:\tX\nUid:\tX\nGid:\tX\n */
   int matched = fscanf(f,
      "Name: %31s State: %c Pid: %u PPid: %u PGid: %u SId: %u Uid: %u Gid: %u",
      nameBuf, &stateBuf, &dummy, &dummy, &dummy, &dummy, &u, &g);
   fclose(f);
   if (matched == 8) {
      *uid = (uid_t) u;
      *gid = (gid_t) g;
   }
}

/* 4 KiB pages on i386 (PUREUNIX_PAGE_SIZE, include/pureunix/vmm.h) —
 * this kernel has no other page size, so this is a real constant, not a
 * platform guess. */
#define PUREUNIX_KB_PER_PAGE 4

void ProcessTable_goThroughEntries(ProcessTable* super) {
   Machine* host = super->super.host;
   PureUnixMachine* phost = (PureUnixMachine*) host;
   const Settings* settings = host->settings;

   DIR* dir = opendir(PROCDIR);
   if (!dir) {
      return;
   }

   char statPath[32];
   char statLine[512];

   struct dirent* entry;
   while ((entry = readdir(dir)) != NULL) {
      const char* name = entry->d_name;
      if (name[0] < '0' || name[0] > '9') {
         continue;
      }
      pid_t pid = (pid_t) atoi(name);
      if (pid <= 0) {
         continue;
      }

      snprintf(statPath, sizeof(statPath), PROCDIR "/%d/stat", (int) pid);
      FILE* f = fopen(statPath, "r");
      if (!f) {
         /* Exited between readdir() and here — normal, just skip it. */
         continue;
      }
      size_t got = fread(statLine, 1, sizeof(statLine) - 1, f);
      fclose(f);
      statLine[got] = '\0';

      PureUnixStat stat;
      if (!parseStat(statLine, &stat)) {
         continue;
      }

      bool preExisting;
      Process* proc = ProcessTable_getProcess(super, pid, &preExisting, PureUnixProcess_new);
      PureUnixProcess* pp = (PureUnixProcess*) proc;

      Process_setPid(proc, pid);
      Process_setParent(proc, (pid_t) stat.ppid);
      Process_setThreadGroup(proc, pid);

      Process_updateComm(proc, stat.comm);

      char cmdline[160];
      readCmdline(pid, cmdline, sizeof(cmdline));
      if (cmdline[0] != '\0') {
         Process_updateCmdline(proc, cmdline, 0, 0);
      } else {
         Process_updateCmdline(proc, stat.comm, 0, 0);
      }

      uid_t uid;
      gid_t gid;
      readIds(pid, &uid, &gid);
      (void) gid;
      proc->st_uid = uid;
      proc->user = UsersTable_getRef(host->usersTable, uid);

      proc->pgrp = (int) stat.pgrp;
      proc->session = (int) stat.session;
      proc->tpgid = 0;
      proc->tty_nr = 0;
      proc->tty_name = NULL;
      proc->processor = 0;

      proc->state = stateFromChar(stat.state);
      /* A task that never execs a real user ELF (fs/procfs.c's
       * approx_vsize_bytes() — kernel/elf.c's mapped_bytes stays 0 for
       * it) is one of this kernel's own permanent service tasks
       * (main_task, a VT session leader, ...) — the closest honest
       * analogue to a Linux kernel thread, so htop brackets it the same
       * way ("[name]"). */
      proc->isKernelThread = (stat.vsize == 0);
      proc->isUserlandThread = false;
      proc->super.show = true;

      super->totalTasks++;
      if (proc->isKernelThread) {
         super->kernelThreads++;
      } else if (proc->isUserlandThread) {
         super->userlandThreads++;
      }
      if (proc->state == RUNNING || proc->state == RUNNABLE) {
         super->runningTasks++;
      }

      proc->priority = stat.priority;
      proc->nice = stat.nice;
      proc->nlwp = 1;

      proc->starttime_ctime = (time_t) stat.starttime;
      Process_fillStarttimeBuffer(proc);

      proc->m_virt = (long) (stat.vsize / 1024);
      proc->m_resident = (long) (stat.rss * PUREUNIX_KB_PER_PAGE);
      proc->percent_mem = host->totalMem
         ? (100.0f * (float) proc->m_resident / (float) host->totalMem)
         : 0.0f;

      proc->minflt = 0;
      proc->majflt = 0;

      /* Real delta-based %CPU — see PureUnixProcess.h's prevUtime
       * comment. Both this kernel's cpu_ticks and htop's own monotonic
       * clock sample here are 100 Hz-equivalent, so no extra scaling is
       * needed once both deltas are in the same tick unit. */
      uint64_t elapsedMs = host->monotonicMs > host->prevMonotonicMs
         ? host->monotonicMs - host->prevMonotonicMs : 0;
      unsigned long long elapsedTicks = elapsedMs / 10;
      if (preExisting && elapsedTicks > 0 && stat.utime >= pp->prevUtime) {
         float pct = (float) (100.0 * (double) (stat.utime - pp->prevUtime) / (double) elapsedTicks);
         proc->percent_cpu = pct > 100.0f ? 100.0f : pct;
      } else {
         proc->percent_cpu = 0.0f;
      }
      pp->prevUtime = stat.utime;
      proc->time = stat.utime; /* already hundredths-of-a-second-equivalent (100 Hz) */

      Process_updateCPUFieldWidths(proc->percent_cpu);

      if (settings->ss->flags & PROCESS_FLAG_CWD) {
         /* No /proc/[pid]/cwd exists yet (fs/procfs.c) — leave unset
          * rather than fabricate one; htop shows a blank CWD column,
          * the correct behavior for genuinely unknown data. */
      }

      proc->super.updated = true;

      if (!preExisting) {
         ProcessTable_add(super, proc);
      }
   }
   closedir(dir);

   (void) phost;
}
