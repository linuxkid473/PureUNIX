# Interrupts

## Overview

Interrupt handling is split across three files:

- `arch/i386/interrupt_stubs.S` — per-vector entry stubs in assembly
- `arch/i386/idt.c` — IDT table management, handler registration, dispatch
- `arch/i386/pic.c` — 8259A PIC configuration and EOI

The IDT has 256 entries. Only entries 0–47 and 128 (0x80) are populated. All others are zero-initialized and will cause an "unhandled interrupt" message if they fire.

---

## IDT Entry Format

```c
typedef struct idt_entry {
    uint16_t base_low;    // bits 0–15 of handler address
    uint16_t selector;    // code segment selector (0x08 = kernel code)
    uint8_t  zero;        // always 0
    uint8_t  flags;       // type + DPL + present bit
    uint16_t base_high;   // bits 16–31 of handler address
} __attribute__((packed)) idt_entry_t;
```

Flags used in PureUnix:

| Flags | Meaning |
|---|---|
| `0x8E` | Present, DPL=0, 32-bit interrupt gate |
| `0xEE` | Present, DPL=3, 32-bit interrupt gate |

DPL=3 on INT 0x80 allows ring-3 user code to invoke it. All other gates have DPL=0.

---

## Interrupt Stubs

`arch/i386/interrupt_stubs.S` defines one stub per vector using two macros:

```asm
ISR_NOERR n   ; for exceptions that do NOT push an error code
ISR_ERR   n   ; for exceptions that DO push an error code
IRQ       n   ; for hardware IRQs (mapped to vector 32+n)
```

**`ISR_NOERR n`** pushes a dummy zero error code first, then the vector number, then jumps to `isr_common_stub`.

**`ISR_ERR n`** pushes only the vector number (the CPU already pushed an error code), then jumps to `isr_common_stub`.

**`IRQ n`** pushes zero (dummy error code) and vector `32+n`, then jumps to `isr_common_stub`.

**`isr128`** (INT 0x80) is hand-coded: pushes zero + 128, jumps to `isr_common_stub`.

### Common Stub

```asm
isr_common_stub:
    pusha                   ; save EAX ECX EDX EBX ESP EBP ESI EDI
    push %ds                ; save segment registers
    push %es
    push %fs
    push %gs

    mov $0x10, %ax          ; load kernel data segment
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp               ; pass pointer to saved registers (interrupt_regs_t *)
    call isr_dispatch
    add $4, %esp

    pop %gs                 ; restore segment registers
    pop %fs
    pop %es
    pop %ds
    popa                    ; restore general registers
    add $8, %esp            ; discard int_no and err_code
    sti                     ; re-enable interrupts
    iret                    ; return: pops EIP, CS, EFLAGS (+ ESP, SS if ring change)
```

The pushed register frame matches `interrupt_regs_t` in `include/pureunix/arch.h`:

```c
typedef struct interrupt_regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  // from pusha
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;             // pushed by CPU
} interrupt_regs_t;
```

---

## Dispatch Logic

`isr_dispatch(interrupt_regs_t *regs)` in `arch/i386/idt.c`:

```
if int_no == 0x80:
    regs->eax = syscall_dispatch(regs)
    return

if handlers[int_no] != NULL:
    handlers[int_no](regs)
else if int_no < 32:
    panic("CPU exception %u (%s)...")
else:
    printf("Unhandled interrupt %u\n")

if 32 <= int_no < 48:
    pic_send_eoi(int_no - 32)
```

Importantly, the EOI is sent **after** the handler returns for IRQs. The syscall path (INT 0x80) does not go through the handler table and does not send an EOI, which is correct because INT 0x80 is a software interrupt.

---

## Exception Vectors (0–31)

| Vector | Name | Error Code |
|---|---|---|
| 0 | Divide by zero | No |
| 1 | Debug | No |
| 2 | Non-maskable interrupt | No |
| 3 | Breakpoint | No |
| 4 | Overflow | No |
| 5 | Bound range exceeded | No |
| 6 | Invalid opcode | No |
| 7 | Device not available | No |
| 8 | Double fault | Yes |
| 9 | Coprocessor segment overrun | No |
| 10 | Invalid TSS | Yes |
| 11 | Segment not present | Yes |
| 12 | Stack-segment fault | Yes |
| 13 | General protection fault | Yes |
| 14 | Page fault | Yes |
| 15 | Reserved | No |
| 16 | x87 floating-point | No |
| 17 | Alignment check | Yes |
| 18 | Machine check | No |
| 19 | SIMD floating-point | No |
| 20–31 | Reserved / vendor | No |

All unhandled exceptions call `panic()`, which prints the exception name, error code, and faulting `EIP`, then halts the CPU.

---

## IRQ Assignments

| IRQ | Vector | Driver | Handler |
|---|---|---|---|
| 0 | 32 | PIT | `pit_irq` — increments `ticks` |
| 1 | 33 | PS/2 keyboard | `keyboard_irq` — reads scancode, queues key |
| 14 | 46 | ATA primary | `ata_irq` — reads status register (clears interrupt) |

IRQs 2–13 and 15 are unhandled. If they fire, `isr_dispatch` prints "Unhandled interrupt N" and sends EOI.

---

## PIC

`arch/i386/pic.c` manages two cascaded 8259A PICs:

| PIC | I/O Ports | IRQs | Mapped vectors |
|---|---|---|---|
| Master (PIC1) | `0x20` (cmd), `0x21` (data) | IRQ 0–7 | INT 32–39 |
| Slave (PIC2) | `0xA0` (cmd), `0xA1` (data) | IRQ 8–15 | INT 40–47 |

**`pic_send_eoi(irq)`**: sends `0x20` (EOI) to PIC1. If `irq >= 8`, also sends EOI to PIC2 first.

**`irq_enable(irq)`**: clears the corresponding bit in the PIC's IMR (interrupt mask register) to unmask the IRQ.

**`irq_disable(irq)`**: sets the bit in the IMR to mask the IRQ.

---

## Syscall Interrupt (INT 0x80)

The INT 0x80 gate is installed in `idt_init` with `flags = 0xEE` (DPL=3). The handler stub is `isr128` which shares `isr_common_stub`. When the vector number is 128, `isr_dispatch` calls `syscall_dispatch(regs)` and places the return value in `regs->eax`.

The calling convention matches the libpure `syscall3` wrapper:

```c
// from user/libpure.c
int syscall3(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80"
        : "=a"(r)
        : "a"(n), "b"(a), "c"(b), "d"(c)
        : "memory");
    return r;
}
```

The kernel reads `regs->eax` (syscall number), `regs->ebx`, `regs->ecx`, `regs->edx` (arguments), and writes the return value back to `regs->eax`.

See `docs/syscalls.md` for the full syscall table.

---

## Handler Registration

Any driver or subsystem can register a handler for any vector:

```c
void interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);
```

The handler table is `static interrupt_handler_t handlers[256]` in `idt.c`. Registering a new handler overwrites the previous one. There is no unregister function.
