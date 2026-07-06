/* Shadow for newlib's own <time.h> (third_party/newlib) — real header,
 * genuine gap: newlib only declares nanosleep() (and the rest of the
 * P1003.1b timer API) inside an #ifdef _POSIX_TIMERS block, which is never
 * defined for a generic bare i686-elf target. PureUNIX has a real
 * SYS_NANOSLEEP (arch/i386/syscall.c, backed by the PIT tick counter —
 * arch/i386/pit.c's pit_sleep()) and user/newlib_syscalls.c's nanosleep()
 * implements it; only the declaration was missing.
 *
 * #include_next finds the next <time.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS). struct timespec
 * itself is already declared unconditionally by newlib's <sys/_timespec.h>
 * (included from its <time.h>), so it's visible here with no extra work.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_TIME_H
#define PUREUNIX_NEWLIB_COMPAT_TIME_H

#include_next <time.h>

int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
