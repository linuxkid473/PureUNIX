/* Assembly entry stub for newlib-linked user programs. This has to be real
 * assembly, not C, for the same reason user/crt0.S is: kernel/elf.c's
 * build_argv_stack() hands the new process a raw initial ESP pointing at
 * [argc][argv][envp] with no return address in front of it (there was never
 * a `call` into _start — the kernel just sets EIP/ESP and `iret`s), so
 * reading those three words has to happen before any compiler-generated
 * prologue (which could itself push registers and move ESP) touches the
 * stack. This reads them, then makes a normal cdecl call into _start_c
 * (user/newlib_crt0.c), which is free to be ordinary C since by that point
 * it's a real call frame like any other.
 */
.section .text
.global _start
.type _start, @function
_start:
    movl (%esp), %eax    /* argc */
    movl 4(%esp), %edx   /* argv */
    movl 8(%esp), %ecx   /* envp */
    pushl %ecx
    pushl %edx
    pushl %eax
    call _start_c
    /* _start_c (see user/newlib_crt0.c) never returns — it ends in exit(),
     * which ends in _exit()'s int $0x81. This is just a safety net. */
.Lhang:
    jmp .Lhang
