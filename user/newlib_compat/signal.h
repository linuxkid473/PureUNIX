/* Shadow for newlib's own <signal.h> (third_party/newlib) — real header,
 * genuine gap: newlib's sys/signal.h declares struct sigaction and
 * sigaction() but is missing several standard SA_* flag values BusyBox
 * references (e.g. SA_RESTART) even in code paths that never actually
 * fire on this kernel — PureUNIX has no real signal delivery yet (see
 * user/newlib_syscalls.c's kill()/signal(), which just self-terminate or
 * fail), so these flags are declaration-only, matching Linux's real bit
 * values in case anything ever inspects them numerically.
 *
 * #include_next finds the next <signal.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SIGNAL_H
#define PUREUNIX_NEWLIB_COMPAT_SIGNAL_H

#include_next <signal.h>

#ifndef SA_RESTART
#define SA_RESTART   0x10000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER   0x40000000
#endif
#ifndef SA_NOMASK
#define SA_NOMASK SA_NODEFER
#endif

#endif
