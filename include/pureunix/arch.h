#ifndef PUREUNIX_ARCH_H
#define PUREUNIX_ARCH_H

#include <pureunix/types.h>

typedef struct interrupt_regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} interrupt_regs_t;

typedef void (*interrupt_handler_t)(interrupt_regs_t *regs);

void arch_init(void);
void gdt_init(void);
void idt_init(void);
void interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);
void isr_dispatch(interrupt_regs_t *regs);

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);

void pit_init(uint32_t hz);
uint64_t pit_ticks(void);
void pit_sleep(uint32_t ms);

void syscall_init(void);
uint32_t syscall_dispatch(interrupt_regs_t *regs);

void context_switch(uint32_t **old_sp, uint32_t *new_sp);

static inline void arch_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void arch_enable_interrupts(void)
{
    __asm__ volatile("sti");
}

static inline void arch_disable_interrupts(void)
{
    __asm__ volatile("cli");
}

#endif
