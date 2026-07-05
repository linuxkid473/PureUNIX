#ifndef VI_COMPAT_SIGNAL_H
#define VI_COMPAT_SIGNAL_H

/* PureUNIX has no signals or job control. vi.c registers a SIGWINCH handler
 * that never fires (no terminal resize events exist), and term.c/cmd.c
 * reference SIGSTOP/SIGINT/kill() for job control and child-process
 * interruption that don't apply here (term_suspend() is a no-op, cmd.c is
 * stubbed). These are no-ops kept only so the call sites still compile. */
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIGINT    2
#define SIGSTOP  19
#define SIGWINCH 28

sighandler_t signal(int signum, sighandler_t handler);
int kill(int pid, int sig);
int raise(int sig);

#endif
