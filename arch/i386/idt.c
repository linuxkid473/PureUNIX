#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/panic.h>
#include <pureunix/signal.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>

typedef struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed)) idt_entry_t;

typedef struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t idtp;

/* Legacy PCI INTx lines are commonly shared between devices (e.g. two PCI
 * functions both routed to IRQ 11) -- when that happens, more than one
 * driver calls interrupt_register_handler() for the *same* vector, and
 * both handlers must run on every interrupt (each checking its own
 * device's pending-interrupt status and doing nothing if it isn't the
 * source -- see e.g. xhci_irq()'s IMAN.IP check in drivers/xhci.c). A
 * fixed-size small array per vector avoids needing a dynamic allocator for
 * something that, in practice, never holds more than a couple of entries;
 * MAX_HANDLERS_PER_VECTOR is a generous bound on how many devices ever
 * realistically share one legacy IRQ line. */
#define MAX_HANDLERS_PER_VECTOR 4
static interrupt_handler_t handlers[256][MAX_HANDLERS_PER_VECTOR];
static uint8_t handler_count[256];

extern void idt_load(uint32_t idt_ptr);

#define ISR_DECL(n) extern void isr##n(void)
ISR_DECL(0); ISR_DECL(1); ISR_DECL(2); ISR_DECL(3); ISR_DECL(4); ISR_DECL(5); ISR_DECL(6); ISR_DECL(7);
ISR_DECL(8); ISR_DECL(9); ISR_DECL(10); ISR_DECL(11); ISR_DECL(12); ISR_DECL(13); ISR_DECL(14); ISR_DECL(15);
ISR_DECL(16); ISR_DECL(17); ISR_DECL(18); ISR_DECL(19); ISR_DECL(20); ISR_DECL(21); ISR_DECL(22); ISR_DECL(23);
ISR_DECL(24); ISR_DECL(25); ISR_DECL(26); ISR_DECL(27); ISR_DECL(28); ISR_DECL(29); ISR_DECL(30); ISR_DECL(31);
extern void irq0(void); extern void irq1(void); extern void irq2(void); extern void irq3(void);
extern void irq4(void); extern void irq5(void); extern void irq6(void); extern void irq7(void);
extern void irq8(void); extern void irq9(void); extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);
extern void isr128(void);
extern void isr129(void);
extern void isr130(void);

static const char *exception_names[] = {
    "divide by zero", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device unavailable",
    "double fault", "coprocessor segment", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "VMM communication", "security", "reserved"
};

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector = selector;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void interrupt_register_handler(uint8_t vector, interrupt_handler_t handler)
{
    if (handler_count[vector] >= MAX_HANDLERS_PER_VECTOR) {
        panic("interrupt_register_handler: vector %u already has %u handlers "
              "(raise MAX_HANDLERS_PER_VECTOR in arch/i386/idt.c)",
              vector, handler_count[vector]);
    }
    handlers[vector][handler_count[vector]++] = handler;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    void (*isrs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (uint8_t i = 0; i < 32; ++i) {
        idt_set_gate(i, (uint32_t)isrs[i], 0x08, 0x8E);
    }

    void (*irqs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (uint8_t i = 0; i < 16; ++i) {
        idt_set_gate(32 + i, (uint32_t)irqs[i], 0x08, 0x8E);
    }

    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
    idt_set_gate(0x81, (uint32_t)isr129, 0x08, 0xEE);
    idt_set_gate(0x82, (uint32_t)isr130, 0x08, 0xEE);
    idt_load((uint32_t)&idtp);
}

/* The single safe point to act on a pending forced-preemption request
 * (arch/i386/pit.c's pit_take_need_resched()) or, from M5 on, a pending
 * signal: right before iret-ing back to ring 3, never mid-kernel-code.
 * `(regs->cs & 3) == 3` is exactly "this trap is about to return to a
 * ring-3 task" — the same boundary isr_common_stub's own `sti` already
 * treats as "safe to leave interrupts enabled from here on". Calling
 * task_yield() here (ordinary kernel-mode code by this point, not IRQ
 * context — isr_dispatch() itself is not the raw handler) is exactly how
 * SYS_NANOSLEEP already blocks, so this is a well-exercised path, not a
 * new kind of call. */
/* Ordering contract: forced preemption is checked (and, if it fires, the
 * yield-away-and-eventually-back-again fully completes) *before* signal
 * delivery is evaluated on the same return — by the time check_preempt()
 * returns, task_current() is guaranteed to be this same trap's own task
 * again (task_yield() only ever resumes a suspended call site once that
 * exact task is rescheduled), so checking signals second still sees an
 * up-to-date, correct pending_signals for the task actually about to
 * resume ring 3. Reversing the order would risk redirecting `regs` into
 * a signal handler for a task that's about to be preempted away again
 * immediately, which — while not unsafe — would be a needlessly
 * confusing interleaving to reason about. */
static void ring3_return_hook(interrupt_regs_t *regs)
{
    if ((regs->cs & 3) != 3) {
        return;
    }
    if (pit_take_need_resched()) {
        task_yield();
    }
    signal_deliver_pending(regs);

    /* Safety net: a signal delivered synchronously — e.g. a default-
     * terminate/stop action from kernel/signal.c's signal_send()/
     * signal_send_pgrp() that happened to target this same task (a
     * keyboard-generated Ctrl+C/Ctrl+Z reaching the interrupted task
     * itself as a member of the signaled foreground process group —
     * kernel/vt.c's vt_input_push(), M6) — already changed this task's
     * own state directly. It must stop running, not fall through to
     * iret-ing back into ring 3 on a stale assumption that it's still
     * runnable. Subsumes the same check SYS_KILL's self-target case
     * (arch/i386/syscall.c) already does explicitly; kept there too as
     * belt-and-suspenders, this is the generic catch-all. */
    task_t *t = task_current();
    if (t && t->state != TASK_RUNNING) {
        if (regs->int_no == 0x80) {
            /* Ordinary C code by this point, not a raw IRQ handler —
             * exactly the SYS_NANOSLEEP-proven-safe context task_yield()
             * already blocks from. */
            task_yield();
            if (t->state == TASK_ZOMBIE) {
                for (;;) {
                    arch_halt();
                }
            }
        } else {
            /* Reached via a raw IRQ's own dispatch (e.g. the keyboard
             * IRQ delivering a signal straight from vt_input_push()) —
             * defer the actual yield to the next timer tick rather than
             * adding a second, unproven task_yield() call site alongside
             * the one PIT-triggered preemption above already exercises.
             * See pit_force_resched()'s own comment. */
            pit_force_resched();
        }
    }
}

void isr_dispatch(interrupt_regs_t *regs)
{
    if (regs->int_no == 0x80) {
        regs->eax = syscall_dispatch(regs);
        ring3_return_hook(regs);
        return;
    }

    if (regs->int_no == 0x81) {
        /* Process-termination trap: not part of the public syscall ABI.
         * task_exit() never returns for the exiting task — it context
         * switches away to whoever is waiting in task_join(). */
        task_exit((int)regs->ebx);
        return;
    }

    if (regs->int_no == 0x82) {
        /* Sigreturn trap: not part of the public syscall ABI — see
         * arch/i386/signal.c. */
        signal_handle_sigreturn(regs);
        return;
    }

    if (handler_count[regs->int_no] > 0) {
        for (uint8_t i = 0; i < handler_count[regs->int_no]; ++i) {
            handlers[regs->int_no][i](regs);
        }
    } else if (regs->int_no < 32) {
        if (regs->int_no == 14) {
            /* Page fault: CR2 holds the faulting linear address (the CPU's
             * own contract, independent of anything the trap frame itself
             * carries) -- essential for diagnosing *what* was being
             * accessed, not just *where the code was* (regs->eip alone
             * leaves "wrote through a bad pointer" and "jumped to garbage"
             * indistinguishable). This kernel has no per-process fault
             * isolation yet (any ring-3 page fault panics the whole
             * system, same as a kernel one), so this is the only diagnostic
             * a ring-3 crash like a stack overflow or a bad pointer
             * ever gets. */
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            panic("CPU exception %u (%s), err=%x eip=%p cr2=%p",
                  regs->int_no, exception_names[regs->int_no], regs->err_code,
                  (void *)regs->eip, (void *)cr2);
        }
        if (regs->int_no == 6) {
            /* Invalid opcode: same reasoning as vector 14's CR2 dump just
             * above -- the bytes actually at eip (not what any on-disk ELF
             * copy says should be there) are the one diagnostic that can
             * distinguish "jumped to garbage" from "the CPU genuinely
             * can't decode legitimate code here", and this kernel has no
             * per-process fault isolation to fall back on either way.
             * Real precedent: this exact dump is what found a genuine
             * memory-layout bug during the Qt QPA port (task_t.heap_base's
             * own comment, include/pureunix/task.h) -- a fixed HEAP_VA
             * offset too small for a large program's own .text let an
             * ordinary heap write (QImage::fill()) land inside still-live
             * code, which only surfaced later as this exact exception when
             * execution reached the clobbered bytes. */
            const uint8_t *p = (const uint8_t *)regs->eip;
            printf("bytes at eip:");
            for (int i = 0; i < 16; i++) {
                printf(" %02x", p[i]);
            }
            printf("\n");
        }
        panic("CPU exception %u (%s), err=%x eip=%p",
              regs->int_no, exception_names[regs->int_no], regs->err_code, (void *)regs->eip);
    } else {
        printf("Unhandled interrupt %u\n", regs->int_no);
    }

    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi((uint8_t)(regs->int_no - 32));
    }

    ring3_return_hook(regs);
}
