#ifndef PUREUNIX_SIGNAL_H
#define PUREUNIX_SIGNAL_H

/* Signal numbers, matching real Linux/BSD numbering (and, not
 * coincidentally, third_party/newlib/i686-elf/include/sys/signal.h's own
 * values for this target) so a signal number crossing the syscall boundary
 * means the same thing on both sides — no translation table needed, unlike
 * errno's few divergent codes (see docs/syscalls.md's error code table).
 *
 * Only the ones PureUNIX's own kernel-side code (arch/i386/syscall.c's
 * SYS_KILL, shell/builtins.c's `kill` builtin) needs to name are listed
 * here; userspace gets the full set from newlib's own <signal.h>. */
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGKILL 9
#define SIGTERM 15

#endif
