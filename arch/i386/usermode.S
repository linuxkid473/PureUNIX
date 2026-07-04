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

.global enter_usermode_regs
.type enter_usermode_regs, @function
/* void enter_usermode_regs(const interrupt_regs_t *regs) -- never returns.
 * regs has the same layout as arch.h's interrupt_regs_t (the frame
 * isr_common_stub pushes on every trap): gs,fs,es,ds / edi,esi,ebp,esp,
 * ebx,edx,ecx,eax / int_no,err_code / eip,cs,eflags,useresp,ss. Rebuilds
 * the iret frame and register set from it and drops to ring 3 at
 * regs->eip -- used to resume a fork()ed child exactly where its parent's
 * int $0x80 was taken (the caller has already zeroed regs->eax so the
 * child observes fork()'s child-side return value). */
enter_usermode_regs:
    mov 4(%esp), %esi

    pushl 72(%esi)      /* ss */
    pushl 68(%esi)      /* useresp */
    pushl 64(%esi)      /* eflags */
    pushl 60(%esi)      /* cs */
    pushl 56(%esi)      /* eip */

    movw 12(%esi), %ax  /* ds */
    mov %ax, %ds
    movw 8(%esi), %ax   /* es */
    mov %ax, %es
    movw 4(%esi), %ax   /* fs */
    mov %ax, %fs
    movw 0(%esi), %ax   /* gs */
    mov %ax, %gs

    movl 44(%esi), %eax
    movl 40(%esi), %ecx
    movl 36(%esi), %edx
    movl 32(%esi), %ebx
    movl 24(%esi), %ebp
    movl 16(%esi), %edi
    movl 20(%esi), %esi /* esi loaded last: it was our base pointer */
    iret
