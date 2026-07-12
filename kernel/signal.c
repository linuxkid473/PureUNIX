/* kernel/signal.c — signal delivery policy: default actions, SIG_IGN, and
 * deciding when a real handler needs to run. The actual mechanics of
 * invoking a handler in ring 3 (the trampoline, saving/restoring
 * register state) live in arch/i386/signal.c — this file only ever
 * touches task_t fields, never registers or memory outside it. See
 * docs/process-management.md. */
#include <pureunix/signal.h>
#include <pureunix/task.h>
#include <pureunix/vt.h>

/* A task killed here (SIGKILL, or any other signal's default terminate
 * action below) never runs another instruction of its own — no atexit,
 * no SDL_DestroyWindow, no chance to call SYS_SET_GRAPHICS_MODE(0) itself.
 * If it had put its own VT into graphics mode, that VT's console would
 * stay suppressed and (see kernel/vt.c's vt_input_push()) its ASCII input
 * queue would keep silently dropping every keystroke forever — the VT's
 * shell effectively frozen with no way back short of a reboot. Found via
 * a real report: Ctrl+C during a running SDL app (default SIGINT action)
 * reproduced exactly this. Belt-and-suspenders with task_exit()'s own
 * identical cleanup for the voluntary-exit path — this one covers every
 * path that skips task_exit() entirely. */
static void force_graphics_mode_off_if_owned(task_t *target)
{
    if (target->vt_id >= 0 && vt_is_graphics_mode(target->vt_id)) {
        vt_set_graphics_mode(target->vt_id, false);
    }
}

/* Notifies target's parent of a waitpid()-relevant state change (a new
 * zombie, a fresh TASK_STOPPED, or a SIGCONT resume): sends real SIGCHLD
 * (deferred delivery for a real handler, via the usual signal_send()
 * path) *and* wakes anyone genuinely blocked in task_waitpid()'s
 * wait_queue_sleep() on task_t.child_wait — see that field's own comment.
 * signal_send() alone only ever sets a pending-signal bit or defers to
 * the ring3-return boundary; it never reschedules a parent parked here. */
static void notify_parent_of_child_change(task_t *target)
{
    task_t *parent = task_find(target->ppid);
    if (!parent) {
        return;
    }
    signal_send(parent, SIGCHLD);
    wait_queue_wake_all(&parent->child_wait);
}

void signal_send(task_t *target, int sig)
{
    if (!target || sig <= 0 || sig >= 32 || target->state == TASK_ZOMBIE) {
        return;
    }

    if (sig == SIGCONT) {
        if (target->state == TASK_STOPPED) {
            target->state = TASK_RUNNABLE;
            /* Traditional job control: resuming a stopped child also
             * notifies the parent via SIGCHLD (so a shell's wait/fg loop,
             * itself typically blocked in waitpid(WUNTRACED), learns the
             * child is running again) — independent of whatever SIGCONT's
             * own disposition below does. */
            notify_parent_of_child_change(target);
        }
        /* Falls through: POSIX says SIGCONT both resumes a stopped
         * process *and* is still deliverable to a real handler if one is
         * installed — the two are independent. */
    }

    if (sig == SIGSTOP) {
        /* Uncatchable, unignorable — POSIX. No disposition to consult. */
        if (target->state != TASK_STOPPED) {
            target->state = TASK_STOPPED;
            target->stop_signal = SIGSTOP;
            target->stop_reported = false;
            notify_parent_of_child_change(target);
        }
        return;
    }

    if (sig == SIGKILL) {
        /* Uncatchable, unignorable — POSIX. Immediate, synchronous
         * termination regardless of whether target is ever scheduled
         * again; matches the pre-M5 task_kill() behavior this replaces. */
        target->state = TASK_ZOMBIE;
        target->exit_code = -sig;
        force_graphics_mode_off_if_owned(target);
        notify_parent_of_child_change(target);
        return;
    }

    uint32_t handler = target->sig_handlers[sig];
    if (handler == PU_SIG_IGN) {
        return;
    }
    if (handler == PU_SIG_DFL) {
        switch (sig) {
        case SIGCHLD:
        case SIGCONT:
            /* Default action for both is "ignore" (SIGCONT's resume side
             * effect above already happened unconditionally). */
            return;
        case SIGTSTP:
            if (target->state != TASK_ZOMBIE && target->state != TASK_STOPPED) {
                target->state = TASK_STOPPED;
                target->stop_signal = SIGTSTP;
                target->stop_reported = false;
                notify_parent_of_child_change(target);
            }
            return;
        default:
            /* Default action for every other signal this kernel sends
             * (SIGHUP/SIGINT/SIGQUIT/SIGTERM, and any future addition) is
             * terminate — the traditional Unix default for anything not
             * specially cased above. */
            target->state = TASK_ZOMBIE;
            target->exit_code = -sig;
            force_graphics_mode_off_if_owned(target);
            notify_parent_of_child_change(target);
            return;
        }
    }

    /* A real handler is installed. Invoking it requires `target` to
     * actually be executing in ring 3 — deferred to the next time it
     * reaches isr_dispatch()'s ring3-return boundary (arch/i386/idt.c),
     * which calls arch/i386/signal.c's signal_deliver_pending(). Blocked
     * signals still accumulate here (POSIX: a blocked signal stays
     * pending, it isn't dropped) — signal_deliver_pending() is what
     * actually respects blocked_signals. */
    target->pending_signals |= (1u << sig);
}

typedef struct {
    uint32_t pgid;
    int sig;
} pgrp_signal_ctx_t;

static void send_if_in_pgrp(const task_t *t, void *ctx)
{
    pgrp_signal_ctx_t *c = (pgrp_signal_ctx_t *)ctx;
    if (t->pgid == c->pgid) {
        /* task_list() only hands back a const view (kernel/task.c) — the
         * underlying task_t is never actually const-qualified storage, so
         * this cast is safe; signal_send() genuinely does need to mutate
         * the target. */
        signal_send((task_t *)t, c->sig);
    }
}

void signal_send_pgrp(uint32_t pgid, int sig)
{
    pgrp_signal_ctx_t ctx = { .pgid = pgid, .sig = sig };
    task_list(send_if_in_pgrp, &ctx);
}

bool signal_is_deliverable(const task_t *t)
{
    if (!t || t->active_signal != 0) {
        return false;
    }
    return (t->pending_signals & ~t->blocked_signals) != 0;
}
