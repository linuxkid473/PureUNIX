#ifndef PUREUNIX_WAIT_H
#define PUREUNIX_WAIT_H

/* Real sleep/wake wait queues, replacing the old
 * `while (cond) { task_yield(); arch_halt(); }` spin-loop pattern used
 * throughout the kernel (pit_sleep(), vt_input_getkey(), pipe read/write).
 * That pattern never actually blocked a task — it just kept re-scheduling
 * it every round for nothing, and TASK_SLEEPING (include/pureunix/task.h)
 * was defined but never set. See docs/process-management.md.
 *
 * A wait_queue_t is an intrusive singly-linked list of task_t's threaded
 * through task_t.wait_next — no separate allocation per sleeper. Safe to
 * embed by value anywhere a "something may be waiting on this" list is
 * needed (a pipe_buf_t, a per-VT keyboard queue, a global timer list, a
 * parent's list of children to reap, ...). Zero-initialized is a valid
 * empty queue. */
#include <pureunix/types.h>

struct task;

typedef struct wait_queue {
    struct task *head;
    struct task *tail;
} wait_queue_t;

typedef bool (*wait_predicate_t)(void *ctx);

/* Blocks the calling task until predicate(ctx) returns true, or returns
 * immediately if it's already true. The check-and-block is atomic with
 * respect to any wait_queue_wake_one()/wait_queue_wake_all() racing in
 * from IRQ context (interrupts are held off across the check + enqueue),
 * so a wakeup that happens to land in that exact window is never lost —
 * the caller is guaranteed to see it on its very next predicate check
 * after being woken.
 *
 * Like every other blocking primitive in this kernel: if the caller was
 * reached through int $0x80, it MUST have already called
 * arch_enable_interrupts() before calling this — see
 * drivers/tty.c's tty_read() for the canonical example and
 * docs/process-management.md for why (syscalls enter with interrupts
 * masked; nothing could ever wake a sleeper otherwise). */
void wait_queue_sleep(wait_queue_t *wq, wait_predicate_t predicate, void *ctx);

/* Wakes at most one sleeper (FIFO), if any is queued. Safe to call from
 * IRQ context (does not touch the caller's interrupt-enable state) or
 * from ordinary kernel/syscall code. */
void wait_queue_wake_one(wait_queue_t *wq);

/* Wakes every sleeper currently queued. Same IRQ-context safety as
 * wait_queue_wake_one(). */
void wait_queue_wake_all(wait_queue_t *wq);

#endif
