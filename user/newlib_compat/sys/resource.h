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

#endif
