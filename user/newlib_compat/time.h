/* Shadow for newlib's own <time.h> (third_party/newlib) — real header,
 * genuine gap: newlib only declares nanosleep() (and the rest of the
 * P1003.1b timer API, including clock_gettime()/clock_getres() AND the
 * CLOCK_MONOTONIC/CLOCK_REALTIME macros themselves) inside an
 * #ifdef _POSIX_TIMERS/__rtems__ block, which is never defined for a
 * generic bare i686-elf target. PureUNIX has real implementations behind
 * all of these (arch/i386/syscall.c's SYS_NANOSLEEP/SYS_GET_TICKS_MS/
 * SYS_GETTIMEOFDAY, wrapped by user/newlib_syscalls.c's nanosleep()/
 * clock_gettime()/clock_getres() — see that file's comment on
 * clock_gettime() for exactly which syscall backs which clockid_t); only
 * the declarations/macros were missing. First hit by Qt Core's
 * QElapsedTimer (src/corelib/kernel/qelapsedtimer_unix.cpp) — that file
 * guards its own real-clock_gettime() path behind `#ifdef CLOCK_MONOTONIC`
 * and silently falls back to a cruder gettimeofday()-based timer without
 * it, so the macros need to be genuinely visible, not just the function
 * prototypes, for Qt to actually take the better path. Values match
 * newlib's own (gated-away) definitions exactly, so a program built
 * against this shim behaves identically to one that could see newlib's
 * real gated block.
 *
 * #include_next finds the next <time.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS). struct timespec
 * itself is already declared unconditionally by newlib's <sys/_timespec.h>
 * (included from its <time.h>), so it's visible here with no extra work.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_TIME_H
#define PUREUNIX_NEWLIB_COMPAT_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME (1)
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC (4)
#endif

int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t clock_id, struct timespec *tp);
int clock_getres(clockid_t clock_id, struct timespec *res);


#ifdef __cplusplus
}
#endif
#endif
