#ifndef PUREUNIX_SIGNAL_H
#define PUREUNIX_SIGNAL_H

/* Signal numbers, matching third_party/newlib/i686-elf/include/sys/signal.h's
 * own values for this target exactly (verified directly against that header,
 * not assumed) so a signal number crossing the syscall boundary means the
 * same thing on both sides — no translation table needed, unlike errno's few
 * divergent codes (see docs/syscalls.md's error code table). This target's
 * newlib falls into that header's generic `#elif !defined(SIGTRAP)` /
 * trailing `#else` branches (not the AM29K/GO32/rtems/svr4-specific ones),
 * which is classic *BSD* numbering for the job-control signals, not Linux's
 * — SIGCHLD=20 here, not 17. Getting this wrong would silently desync
 * BusyBox ash's own compiled-in signal numbers (it links against newlib's
 * <signal.h>, not this file) from the kernel's.
 *
 * Only the ones PureUNIX's own kernel-side code (arch/i386/syscall.c's
 * SYS_KILL, kernel/signal.c, shell/builtins.c's `kill` builtin) needs to
 * name are listed here; userspace gets the full set from newlib's own
 * <signal.h>. */
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGKILL 9
#define SIGTERM 15
#define SIGSTOP 17
#define SIGTSTP 18
#define SIGCONT 19
#define SIGCHLD 20

/* Matches newlib's own SIG_DFL=0/SIG_IGN=1 exactly (third_party/newlib/
 * i686-elf/include/signal.h) — task_t.sig_handlers[] entries use these
 * same two sentinel values directly, no translation needed. */
#define PU_SIG_DFL 0u
#define PU_SIG_IGN 1u

/* Matches newlib's own sys/signal.h SIG_SETMASK/SIG_BLOCK/SIG_UNBLOCK
 * exactly — SYS_SIGPROCMASK's `how` argument. */
#define PU_SIG_SETMASK 0
#define PU_SIG_BLOCK   1
#define PU_SIG_UNBLOCK 2

#include <pureunix/arch.h>
#include <pureunix/task.h>
#include <pureunix/types.h>

/* The struct exchanged with SYS_SIGACTION — deliberately much smaller
 * than newlib's own struct sigaction (no siginfo/sa_sigaction variant, no
 * SA_* flags, no sa_mask support — see docs/process-management.md's scope
 * notes). user/newlib_syscalls.c translates to/from newlib's own struct
 * sigaction. */
typedef struct pu_sigaction {
    uint32_t handler; /* PU_SIG_DFL, PU_SIG_IGN, or a ring-3 function pointer */
} pu_sigaction_t;

/* Sends signal `sig` to `target` — applies a default action synchronously
 * (terminate/stop/continue/ignore, matching traditional Unix semantics)
 * if no real handler is installed, or marks it pending for asynchronous
 * delivery via the trampoline (arch/i386/signal.c) the next time `target`
 * is actually scheduled, if one is. No-op if target is NULL or already a
 * zombie. SIGKILL/SIGSTOP can never be caught or ignored — POSIX. */
void signal_send(task_t *target, int sig);
/* Sends `sig` to every live task whose pgid == pgid — the kernel-side
 * backing for kill(-pgid, sig) and keyboard-generated signals
 * (Ctrl+C/Ctrl+Z/Ctrl+\, kernel/vt.c's fg_pgid — see M6). */
void signal_send_pgrp(uint32_t pgid, int sig);
/* True if `t` has a real-handler signal queued and ready to fire (not
 * blocked, no handler already running) — used by kernel/wait.c's
 * wait_queue_sleep() to abort a long, in-progress wait early so the
 * signal actually gets a chance to run at the next syscall-return
 * boundary, rather than only being checked once the *entire* wait
 * naturally completes (which, for something like a long nanosleep(),
 * could be seconds away — see docs/process-management.md). */
bool signal_is_deliverable(const task_t *t);

/* arch/i386/signal.c — the register/memory mechanics (see that file's
 * header comment for the full design). */

/* Maps the shared, read+exec-only signal trampoline into a freshly
 * created address space at SIGNAL_TRAMPOLINE_VA (vmm.h) — called once by
 * kernel/elf.c's elf_load_into() for every exec(), so every process has
 * one; fork() inherits it for free via the normal address-space-copy
 * path (vmm_fork_address_space()). */
void signal_map_trampoline(uint32_t pd_phys);
/* Called from arch/i386/idt.c's isr_dispatch() at the ring3-return
 * boundary (same one preemption uses) — if the current task has a
 * deliverable pending signal with a real handler installed, redirects
 * *regs to resume execution in the trampoline instead of where it was
 * about to return to. No-op otherwise. */
void signal_deliver_pending(interrupt_regs_t *regs);
/* The int $0x82 trap handler (not part of the public syscall ABI, same
 * precedent as int $0x81's task-termination trap) — restores the trap
 * frame signal_deliver_pending() saved before redirecting into the
 * trampoline, so execution resumes exactly where the signal interrupted
 * it. */
void signal_handle_sigreturn(interrupt_regs_t *regs);

#endif
