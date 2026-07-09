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
/* Adds `handler` to the (small, fixed-capacity) list of handlers invoked
 * whenever `vector` fires -- not a single-slot "set the handler" call.
 * Legacy PCI INTx lines are commonly shared between devices, so more than
 * one driver may register on the same vector; every registered handler
 * runs on each interrupt, so a handler for a device on a possibly-shared
 * vector must check its own pending-interrupt status first and do nothing
 * if it isn't the source (see drivers/xhci.c's xhci_irq() for an example). */
void interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);
void isr_dispatch(interrupt_regs_t *regs);

/* Points the hardware task state segment's ring-0 stack at esp0, so the
 * next ring3 -> ring0 trap (syscall or exception) lands on the given
 * kernel stack instead of whatever the previous running task left there. */
void tss_set_kernel_stack(uint32_t esp0);

/* Drops to CPL3 and starts executing at `entry` on `user_stack` (which
 * must already be present + PAGE_USER mapped). Never returns. */
void enter_usermode(uint32_t entry, uint32_t user_stack) __attribute__((noreturn));

/* Restores every field of *regs (same layout isr_common_stub pushes on
 * entry) and irets into ring 3 at regs->eip. Used to resume a fork()ed
 * child exactly where its parent's int $0x80 was taken. Never returns. */
void enter_usermode_regs(const interrupt_regs_t *regs) __attribute__((noreturn));

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
