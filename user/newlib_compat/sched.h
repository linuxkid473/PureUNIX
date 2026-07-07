/* Shadow for newlib's own <sched.h> (third_party/newlib) — real header,
 * genuine gap: sched_yield() is declared only under
 * `#if defined(_POSIX_THREADS) || defined(_POSIX_PRIORITY_SCHEDULING)`,
 * neither of which this bare i686-elf target defines (no threads, no
 * priority-scheduling feature macro), so BusyBox's `less` (which calls it
 * directly while polling for more input) fails to compile with an
 * implicit-declaration error. user/newlib_syscalls.c's sched_yield() is a
 * real implementation (SYS_YIELD, the same cooperative reschedule used
 * everywhere else in this kernel), not a stub — this header just makes the
 * declaration visible unconditionally.
 *
 * #include_next finds the next <sched.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SCHED_H
#define PUREUNIX_NEWLIB_COMPAT_SCHED_H

#include_next <sched.h>

int sched_yield(void);

#endif
