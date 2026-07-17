/* Shadow for newlib's own <sys/wait.h> (third_party/newlib) — real header,
 * genuine gap: it defines WIFEXITED/WIFSIGNALED/WTERMSIG/WEXITSTATUS but not
 * WCOREDUMP, which BusyBox's ash (sprint_status48()) references
 * unconditionally. PureUNIX has no core-dump support at all (no signal
 * handler dispatch, no core file writing — see docs/syscalls.md's SYS_KILL
 * section), so this is always false.
 *
 * #include_next finds the next <sys/wait.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_WAIT_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <sys/wait.h>

#ifndef WCOREDUMP
#define WCOREDUMP(w) 0
#endif


#ifdef __cplusplus
}
#endif
#endif
