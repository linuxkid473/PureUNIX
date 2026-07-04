/* int $0x81 is a kernel-internal process-termination trap, deliberately
 * distinct from the public int $0x80 / SYS_* syscall gate: SYS_EXIT (see
 * libpure.h) is a documented no-op pass-through, not a real exit(), so
 * ending this process after main() returns needs a different mechanism.
 * See arch/i386/idt.c and arch/i386/interrupt_stubs.S (isr129). */

.section .text
.global _start
.type _start, @function
_start:
    call main
    movl %eax, %ebx
    int $0x81
.Lhang:
    /* int $0x81 never returns; this is just a safety net. */
    jmp .Lhang
