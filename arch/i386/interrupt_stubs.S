.section .text

.macro ISR_NOERR n
.global isr\n
.type isr\n, @function
isr\n:
    cli
    pushl $0
    pushl $\n
    jmp isr_common_stub
.endm

.macro ISR_ERR n
.global isr\n
.type isr\n, @function
isr\n:
    cli
    pushl $\n
    jmp isr_common_stub
.endm

.macro IRQ n
.global irq\n
.type irq\n, @function
irq\n:
    cli
    pushl $0
    pushl $(32 + \n)
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

.global isr128
.type isr128, @function
isr128:
    cli
    pushl $0
    pushl $128
    jmp isr_common_stub

/* Kernel-internal process-termination trap (int $0x81) — distinct from
 * the public int $0x80 syscall gate. Used only by user/crt0.S after
 * main() returns; never exposed as a SYS_* number. */
.global isr129
.type isr129, @function
isr129:
    cli
    pushl $0
    pushl $129
    jmp isr_common_stub

.extern isr_dispatch
.global isr_common_stub
.type isr_common_stub, @function
isr_common_stub:
    pusha
    push %ds
    push %es
    push %fs
    push %gs

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp
    call isr_dispatch
    add $4, %esp

    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp
    sti
    iret
