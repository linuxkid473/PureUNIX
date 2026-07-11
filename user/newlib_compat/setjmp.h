/* Shadow for newlib's own <setjmp.h> (third_party/newlib) — newlib DOES
 * provide a real setjmp()/longjmp() (implemented in asm for i386), but
 * this bare target's setjmp.h has no case that declares the BSD `_setjmp`/
 * `_longjmp` variants (normally gated behind host-specific macros nothing
 * here defines) — a real gap, not intentionally omitted.
 *
 * Real upstream Lua's ldo.c picks `_setjmp`/`_longjmp` over plain
 * `setjmp`/`longjmp` whenever LUA_USE_POSIX is defined (its own comment:
 * "in POSIX, try _longjmp/_setjmp (more efficient)") — the BSD variants
 * skip saving/restoring the process's signal mask, which plain
 * setjmp/longjmp must do. On PureUNIX there IS no signal mask to skip:
 * sigprocmask()/sigaction() (user/newlib_syscalls.c) are honest
 * bookkeeping only — no signal ever actually interrupts a running task to
 * begin with (docs/syscalls.md's signal-delivery scope boundary) — so
 * `_setjmp`/`_longjmp` and `setjmp`/`longjmp` are behaviorally identical
 * here, not an approximation. Aliasing them via macro is the correct
 * implementation, not a shortcut. #include_next finds newlib's real
 * setjmp.h (this directory is searched first — see NEWLIB_CFLAGS in the
 * Makefile), same trick user/newlib_compat/stdlib.h uses.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SETJMP_H
#define PUREUNIX_NEWLIB_COMPAT_SETJMP_H

#include_next <setjmp.h>

#ifndef _setjmp
#define _setjmp(env) setjmp(env)
#endif
#ifndef _longjmp
#define _longjmp(env, val) longjmp(env, val)
#endif

#endif
