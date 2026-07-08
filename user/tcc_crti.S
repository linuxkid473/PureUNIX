/* crti.o for TCC-compiled programs (see docs/tcc-port.md). Opens the
 * _init/_fini function bodies that crtn.o (user/tcc_crtn.S) closes — the
 * standard glibc-style crti.S/crtn.S split, so the object files concatenate
 * into two complete (if empty) functions with nothing in between. PureUNIX
 * has no dynamic linker to ever call DT_INIT/_init, and TCC's own crt1.o
 * (reused from user/newlib_crt0.S/.c — see docs/tcc-port.md) never calls
 * _fini either, so these exist purely to satisfy the linker's structural
 * expectation that crti.o/crtn.o exist and pair up; there is no
 * constructor/destructor (.init_array/.fini_array) support here.
 */
.section .init
.global _init
.type _init, @function
_init:
    pushl %ebp
    movl %esp, %ebp

.section .fini
.global _fini
.type _fini, @function
_fini:
    pushl %ebp
    movl %esp, %ebp
