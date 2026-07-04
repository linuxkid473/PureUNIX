.section .text
.global enter_usermode
.type enter_usermode, @function
/* void enter_usermode(uint32_t entry, uint32_t user_stack) -- never returns.
 * Builds the iret frame that drops CPL from 0 to 3: user data selector
 * (0x20 | RPL3) in the segment registers, then ss:esp, eflags, cs:eip for
 * the target ring3 entry point. */
enter_usermode:
    mov 4(%esp), %eax   /* entry */
    mov 8(%esp), %ecx   /* user_stack */

    mov $0x23, %bx      /* user data selector | RPL3 */
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs

    pushl $0x23         /* ss */
    pushl %ecx          /* esp */
    pushf
    popl %edx
    orl $0x200, %edx    /* force IF=1 */
    pushl %edx          /* eflags */
    pushl $0x1B         /* cs = user code selector | RPL3 */
    pushl %eax          /* eip */
    iret
