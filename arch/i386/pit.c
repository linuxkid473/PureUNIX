#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/task.h>
#include <pureunix/wait.h>

static volatile uint64_t ticks;
static uint32_t pit_hz = 100;

/* Everything currently blocked in pit_sleep() waits on this single shared
 * queue — woken on every tick and re-checks its own deadline via the
 * predicate passed to wait_queue_sleep(), rather than one queue per
 * sleeper (there's no way to target a wake_one() at a specific deadline
 * anyway, and the sleeper counts here are small). */
static wait_queue_t sleep_wq;

/* Minimal preemption: this kernel is otherwise purely cooperative (see
 * docs/scheduler.md), so a CPU-bound task that never calls task_yield()
 * itself (a busy `yes > /dev/null &`-style job, or just a bug) would
 * starve every other task — including every other VT's shell — forever.
 * PREEMPT_QUANTUM_TICKS (50ms at the default 100Hz) is deliberately
 * coarse: this is a starvation backstop, not a real time-sharing
 * scheduler — see docs/process-management.md's scope notes. */
#define PREEMPT_QUANTUM_TICKS 5
static volatile bool need_resched;

static void pit_irq(interrupt_regs_t *regs)
{
    (void)regs;
    ticks++;
    task_t *t = task_current();
    if (t) {
        t->cpu_ticks++;
        t->quantum_ticks++;
        if (t->quantum_ticks >= PREEMPT_QUANTUM_TICKS && task_other_runnable_exists()) {
            /* Deliberately does NOT call task_yield()/context_switch()
             * here — this is IRQ context, and context switching was
             * never designed to be reentrant from inside a raw handler
             * (see docs/process-management.md's interrupt-gate hazard).
             * Just flag it; arch/i386/idt.c's isr_dispatch() performs
             * the actual yield once, at the safe ring3-return boundary,
             * the same pattern used for signal delivery (M5). */
            need_resched = true;
        }
    }
    /* Wakes every sleeper to re-check its own deadline predicate; anyone
     * whose deadline hasn't arrived yet just re-enqueues itself inside
     * wait_queue_sleep()'s loop. Cheap enough at this sleeper count and
     * far simpler/more obviously correct than maintaining a sorted
     * per-deadline structure. */
    wait_queue_wake_all(&sleep_wq);
}

bool pit_take_need_resched(void)
{
    bool r = need_resched;
    need_resched = false;
    return r;
}

void pit_force_resched(void)
{
    /* Same deferred-flag mechanism as pit_irq()'s own preemption check
     * above, reused by arch/i386/idt.c's ring3_return_hook() for its
     * "current is no longer TASK_RUNNING" safety net — that check can
     * itself be reached from *any* IRQ's dispatch (not just the PIT's own,
     * e.g. a keyboard-generated signal that happens to target whichever
     * task was interrupted — kernel/vt.c's vt_input_push(), M6), and only
     * the PIT-IRQ-triggered call to task_yield() at this exact boundary
     * has actually been exercised/proven safe so far. Flagging here and
     * letting the *next* tick (at most ~10ms away) perform the real
     * task_yield() keeps every actual context switch funneled through
     * that one proven path, rather than adding a second, untested
     * call site for it. */
    need_resched = true;
}

void pit_init(uint32_t hz)
{
    pit_hz = hz ? hz : 100;
    uint32_t divisor = 1193182 / pit_hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    interrupt_register_handler(32, pit_irq);
    irq_enable(0);
}

uint64_t pit_ticks(void)
{
    return ticks;
}

typedef struct {
    uint64_t deadline;
} sleep_ctx_t;

static bool deadline_reached(void *ctx)
{
    return ticks >= ((sleep_ctx_t *)ctx)->deadline;
}

void pit_sleep(uint32_t ms)
{
    /* Callers reached through int $0x80 (SYS_NANOSLEEP, SYS_PING) must
     * have already called arch_enable_interrupts() themselves before
     * calling this — see include/pureunix/wait.h's wait_queue_sleep()
     * invariant and arch/i386/syscall.c's SYS_NANOSLEEP/SYS_PING cases. */
    sleep_ctx_t ctx = { .deadline = ticks + ((uint64_t)ms * pit_hz + 999) / 1000 };
    wait_queue_sleep(&sleep_wq, deadline_reached, &ctx);
}
