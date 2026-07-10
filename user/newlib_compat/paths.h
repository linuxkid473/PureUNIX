/* Shadow for newlib's own <paths.h> (third_party/newlib) — real header,
 * genuine gap: newlib's vendored copy (see that header) defines
 * _PATH_DEV/_PATH_DEVNULL/_PATH_DEVZERO/_PATH_BSHELL but not _PATH_TTY,
 * which BusyBox ash's setjobctl() (shell/ash.c, only compiled in with
 * CONFIG_ASH_JOB_CONTROL) needs to open the controlling terminal directly.
 * Matches the real device node arch/i386/syscall.c's /dev/tty interception
 * (SYS_OPEN) already serves.
 *
 * #include_next finds the next <paths.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_PATHS_H
#define PUREUNIX_NEWLIB_COMPAT_PATHS_H

#include_next <paths.h>

#ifndef _PATH_TTY
#define _PATH_TTY "/dev/tty"
#endif

#endif
