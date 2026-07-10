/* kernel/wait.c — real sleep/wake wait queues. See include/pureunix/wait.h
 * for the design rationale and the interrupt-masking invariant every
 * caller must respect. */
#include <pureunix/arch.h>
#include <pureunix/signal.h>
#include <pureunix/task.h>
#include <pureunix/wait.h>

static void wq_enqueue(wait_queue_t *wq, task_t *t)
{
    t->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = t;
    } else {
        wq->head = t;
    }
    wq->tail = t;
}

static task_t *wq_dequeue_head(wait_queue_t *wq)
{
    task_t *t = wq->head;
    if (!t) {
        return NULL;
    }
    wq->head = t->wait_next;
    if (!wq->head) {
        wq->tail = NULL;
    }
    t->wait_next = NULL;
    return t;
}

void wait_queue_sleep(wait_queue_t *wq, wait_predicate_t predicate, void *ctx)
{
    for (;;) {
        uint32_t flags = arch_save_and_disable_interrupts();
        task_t *self = task_current();
        /* A deliverable signal (a real handler queued and ready — see
         * kernel/signal.c's signal_is_deliverable()) aborts a long wait
         * early, the same way a real blocking syscall is interruptible
         * on a real Unix. Without this, a task blocked here for a long,
         * single, uninterrupted stretch (pit_sleep()'s own wait queue is
         * the sharpest example: a 100-second nanosleep()) would never
         * reach a syscall-return boundary for arch/i386/idt.c's
         * ring3_return_hook() to actually fire the signal until the
         * *entire* wait completed on its own — see
         * docs/process-management.md. The predicate itself is still
         * checked first/normally; this only ever fires an *early* exit,
         * never suppresses a legitimate one. */
        /* A default-action signal (e.g. Ctrl+\'s SIGQUIT reaching this
         * task while it was off asleep — kernel/signal.c's signal_send())
         * applies synchronously and directly: it already set self->state
         * to TASK_ZOMBIE/TASK_STOPPED *outside* this loop's own control.
         * Must never re-enqueue and paper back over that with
         * TASK_SLEEPING below — that would silently resurrect a supposed-
         * to-be-dead task's own wait forever, since nothing else here
         * would ever notice the state change again. Returning here
         * instead unwinds normally back to this task's own syscall
         * handler and out to arch/i386/idt.c's ring3_return_hook(), whose
         * own "no longer TASK_RUNNING" safety net is what actually stops
         * it running any further. */
        if (predicate(ctx) || signal_is_deliverable(self) || self->state == TASK_ZOMBIE) {
            arch_restore_interrupts(flags);
            return;
        }
        wq_enqueue(wq, self);
        self->state = TASK_SLEEPING;
        arch_restore_interrupts(flags);

        /* If another task is ready, this switches to it and only returns
         * here once `self` is scheduled again (by which point some waker
         * has already set self->state back to TASK_RUNNABLE and dequeued
         * it — see wait_queue_wake_one/all below). If nothing else is
         * ready, next_ready_task() (kernel/task.c) falls back to
         * returning `self` itself even though it's TASK_SLEEPING, so
         * task_yield() is a no-op here and arch_halt() is what actually
         * parks the CPU until the next interrupt (a waker running in IRQ
         * context, e.g. the PIT tick or a keyboard interrupt) — same
         * halt-for-next-IRQ idiom every other blocking primitive in this
         * kernel already relies on. */
        task_yield();
        arch_halt();
    }
}

void wait_queue_wake_one(wait_queue_t *wq)
{
    uint32_t flags = arch_save_and_disable_interrupts();
    task_t *t = wq_dequeue_head(wq);
    if (t && t->state == TASK_SLEEPING) {
        t->state = TASK_RUNNABLE;
    }
    arch_restore_interrupts(flags);
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    uint32_t flags = arch_save_and_disable_interrupts();
    task_t *t;
    while ((t = wq_dequeue_head(wq)) != NULL) {
        if (t->state == TASK_SLEEPING) {
            t->state = TASK_RUNNABLE;
        }
    }
    arch_restore_interrupts(flags);
}
