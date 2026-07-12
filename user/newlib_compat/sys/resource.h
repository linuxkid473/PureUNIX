/* newlib has no <sys/resource.h> at all for a bare i686-elf target (it's
 * genuinely absent, not just incomplete — there is no #include_next target
 * to shadow, unlike most of this directory's other headers). PureUNIX has
 * no resource-limiting model whatsoever (no per-process CPU/memory/fsize
 * caps enforced anywhere in the kernel), so getrlimit()/setrlimit()
 * (user/newlib_syscalls.c) are honest "always unlimited, nothing enforced"
 * stubs — this only exists so ash's `ulimit` builtin (shell/shell_common.c)
 * links and runs (reporting "unlimited" for everything, same as `ulimit`
 * would in a container with no cgroup limits applied).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_RESOURCE_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_RESOURCE_H

#include <sys/time.h>
#include <sys/types.h>

typedef unsigned long rlim_t;

#define RLIM_INFINITY ((rlim_t)-1)
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_CPU   0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA  2
#define RLIMIT_STACK 3
#define RLIMIT_CORE  4
#define RLIMIT_RSS   5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS    9
#define RLIM_NLIMITS 10

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

/* nice()/renice() — unlike getrlimit()/setrlimit() above, this is real:
 * SYS_SETPRIORITY/SYS_GETPRIORITY (docs/syscalls.md) genuinely bias
 * kernel/task.c's scheduler (see next_ready_task()'s own comment for the
 * exact, deliberately simple scheme). Only PRIO_PROCESS is implemented —
 * PRIO_PGRP/PRIO_USER are accepted (so BusyBox's renice still links and
 * runs) but rejected at the syscall level with -EINVAL, same as a real
 * kernel would for a `which` it doesn't support for a given call. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

/* getrusage(): an honest stub, same category as getrlimit()/setrlimit()
 * above — PureUNIX's per-task CPU-tick counter (task_t.cpu_ticks,
 * arch/i386/pit.c) is never surfaced through any syscall (times()/
 * sysconf() are the same kind of stub, see docs/libc.md's "Honest
 * stubs"), so this always reports zeroed usage rather than a real
 * measurement. Added for the SQLite port (docs/sqlite-port.md): the
 * sqlite3 CLI's `.timer on` meta-command calls this to print per-query
 * CPU time; with this stub it links, runs, and prints "0.000000" rather
 * than failing to link at all — a real gap (no accurate CPU-time
 * reporting), not a crash. */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
};
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

int getrusage(int who, struct rusage *usage);

#endif
