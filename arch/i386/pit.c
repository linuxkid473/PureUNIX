#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/task.h>

static volatile uint64_t ticks;
static uint32_t pit_hz = 100;

static void pit_irq(interrupt_regs_t *regs)
{
    ticks++;
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

void pit_sleep(uint32_t ms)
{
    uint64_t end = ticks + ((uint64_t)ms * pit_hz + 999) / 1000;
    while (ticks < end) {
        /* Yield first so another ready task (e.g. a different VT's shell
         * while this one sleeps between `ping` packets — see
         * docs/scheduler.md) gets a turn; if nothing else is ready,
         * task_yield() is a no-op and the halt below just waits for the
         * next tick instead of spinning. Callers that can be reached via
         * int $0x80 (SYS_NANOSLEEP, SYS_PING) must have already
         * re-enabled interrupts — see arch/i386/syscall.c — or neither the
         * timer tick nor this yield's eventual return could ever happen. */
        task_yield();
        arch_halt();
    }
}
