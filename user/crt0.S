/* int $0x81 is a kernel-internal process-termination trap, deliberately
 * distinct from the public int $0x80 / SYS_* syscall gate: SYS_EXIT (see
 * libpure.h) is a documented no-op pass-through, not a real exit(), so
 * ending this process after main() returns needs a different mechanism.
 * See arch/i386/idt.c and arch/i386/interrupt_stubs.S (isr129). */

/* kernel/elf.c's build_argv_stack() leaves argc at [esp] and argv at
 * [esp+4] on the initial stack it hands to every new process (mirroring
 * what "push argv; push argc" would leave right before a call), so they
 * just need re-pushing in cdecl order for main(argc, argv). Programs
 * declared main(void) (most of user/*.c) simply ignore the two extra
 * stack slots — harmless on cdecl, same as a real libc crt0. */
.section .text
.global _start
.type _start, @function
_start:
    movl (%esp), %eax
    movl 4(%esp), %edx
    pushl %edx
    pushl %eax
    call main
    movl %eax, %ebx
    int $0x81
.Lhang:
    /* int $0x81 never returns; this is just a safety net. */
    jmp .Lhang
