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

/* Repoints the shared ring-3 TLS descriptor (selector 0x33) at `base` —
 * see arch/i386/gdt.c's own comment and task_t.tls_base
 * (include/pureunix/task.h) for the full picture. */
void gdt_set_tls_base(uint32_t base);

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
/* True (and clears the flag) if pit_irq() decided a CPU-bound task has
 * monopolized the CPU for a full quantum with another task genuinely
 * runnable — see arch/i386/pit.c's minimal-preemption comment. Checked
 * (and acted on with a single task_yield()) at isr_dispatch()'s
 * ring3-return boundary, arch/i386/idt.c. */
bool pit_take_need_resched(void);
/* Flags the same deferred-reschedule mechanism pit_take_need_resched()
 * consumes, without itself calling task_yield() — see arch/i386/pit.c's
 * own comment on this function for why. */
void pit_force_resched(void);

void syscall_init(void);
uint32_t syscall_dispatch(interrupt_regs_t *regs);

void context_switch(uint32_t **old_sp, uint32_t *new_sp);

/* Enables real hardware x87/SSE support (CR0.EM=0/MP=1, CR4.OSFXSR=1,
 * CR4.OSXMMEXCPT=1) and captures a valid "freshly reset FPU" FXSAVE
 * image for fpu_init_task_state() to seed every new task with -- see
 * arch/i386/fpu.c and task_t.fpu_state's own comment
 * (include/pureunix/task.h) for why this exists at all. Must run once,
 * before tasking_init() creates any task. */
void fpu_init(void);

/* Copies fpu_init()'s captured default state into `dst` (task_t.fpu_state,
 * which callers align first via fpu_state_area()) -- called once per task
 * by task_alloc() (kernel/task.c) so a task that has never yet been
 * context-switched away from still has a legal image ready for the very
 * first FXRSTOR when it's about to run as `next`. */
void fpu_init_task_state(uint8_t *dst);

/* Rounds a task_t.fpu_state[512+16] buffer up to the 16-byte boundary
 * FXSAVE/FXRSTOR's memory operand requires -- shared by kernel/task.c
 * (task_alloc()/task_yield()) and arch/i386/fpu.c itself. */
static inline uint8_t *fpu_state_area(uint8_t *raw)
{
    return (uint8_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
}

/* Real FXSAVE/FXRSTOR — `area` must already be 16-byte aligned (see
 * fpu_state_area() above). The one place these actually run is
 * kernel/task.c's task_yield(), around every context_switch(). */
static inline void fpu_save(uint8_t *area)
{
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

static inline void fpu_restore(uint8_t *area)
{
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

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

/* Saves EFLAGS (which captures the current IF state) and clears IF,
 * returning the saved value for a matching arch_restore_interrupts() —
 * the standard save/cli .. restore idiom for a critical section that must
 * work correctly whether the caller already had interrupts enabled
 * (ordinary kernel/syscall code) or already had them disabled (code
 * called from inside an IRQ handler, which must never come out of a
 * nested critical section with interrupts re-enabled early — only the
 * common ISR stub's own `sti` right before `iret` may do that). See
 * kernel/wait.c's wait queues, the only current user. */
static inline uint32_t arch_save_and_disable_interrupts(void)
{
    uint32_t flags;
    __asm__ volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void arch_restore_interrupts(uint32_t flags)
{
    __asm__ volatile("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

#endif
