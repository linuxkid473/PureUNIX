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

#ifdef __cplusplus
extern "C" {
#endif

#include_next <signal.h>

/* newlib's sys/signal.h defines sigaddset/sigdelset/sigemptyset/
 * sigfillset/sigismember as BOTH real function declarations AND
 * function-like macros of the same name (a real, deliberate glibc-style
 * fast-path idiom) — but a macro-shadowed name breaks any C++ call site
 * that qualifies it with `::` (e.g. `::sigemptyset(&mask)`, used by real
 * upstream code like pcmanfm-qt's own signal-handling setup): the
 * preprocessor still expands the macro first, leaving a bare `::`
 * immediately followed by `(`, a hard syntax error ("expected
 * id-expression before '(' token"). Undefining the macros here restores
 * the real function declarations already present in the header above as
 * the only way to call these — real definitions are in
 * user/newlib_syscalls.c. */
#undef sigaddset
#undef sigdelset
#undef sigemptyset
#undef sigfillset
#undef sigismember

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


#ifdef __cplusplus
}
#endif
#endif
