/* crtn.o for TCC-compiled programs — closes the _init/_fini function bodies
 * user/tcc_crti.S opens. See that file's header comment for why these are
 * empty (no PureUNIX dynamic linker ever calls _init; nothing calls _fini
 * either).
 */
.section .init
    movl %ebp, %esp
    popl %ebp
    ret

.section .fini
    movl %ebp, %esp
    popl %ebp
    ret
