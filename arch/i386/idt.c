#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/panic.h>
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
    idt_load((uint32_t)&idtp);
}

void isr_dispatch(interrupt_regs_t *regs)
{
    if (regs->int_no == 0x80) {
        regs->eax = syscall_dispatch(regs);
        return;
    }

    if (regs->int_no == 0x81) {
        /* Process-termination trap: not part of the public syscall ABI.
         * task_exit() never returns for the exiting task — it context
         * switches away to whoever is waiting in task_join(). */
        task_exit((int)regs->ebx);
        return;
    }

    if (handler_count[regs->int_no] > 0) {
        for (uint8_t i = 0; i < handler_count[regs->int_no]; ++i) {
            handlers[regs->int_no][i](regs);
        }
    } else if (regs->int_no < 32) {
        panic("CPU exception %u (%s), err=%x eip=%p",
              regs->int_no, exception_names[regs->int_no], regs->err_code, (void *)regs->eip);
    } else {
        printf("Unhandled interrupt %u\n", regs->int_no);
    }

    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi((uint8_t)(regs->int_no - 32));
    }
}
