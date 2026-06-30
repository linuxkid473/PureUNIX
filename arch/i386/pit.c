#include <pureunix/arch.h>
#include <pureunix/io.h>

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
        arch_halt();
    }
}
