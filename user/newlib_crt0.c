/* C half of the entry stub for newlib-linked user programs — counterpart to
 * user/crt0.S used by libpure-only programs. Unlike crt0.S (which traps
 * straight to int $0x81 with main()'s return value), this calls newlib's
 * exit(), which flushes stdio buffers and runs atexit() handlers before it
 * gets there — necessary because newlib's stdout is fully buffered whenever
 * isatty() would say otherwise, and matters even here where isatty() always
 * returns true (see user/newlib_syscalls.c) once a program registers its own
 * atexit() handlers. exit() ends by calling _exit(), also implemented in
 * user/newlib_syscalls.c, which performs the actual int $0x81 trap.
 *
 * _start_c is called from user/newlib_crt0.S's _start with the real
 * argc/argv/envp that kernel/elf.c's build_argv_stack() placed on this
 * process's initial stack (see that file's comment on why this can't just
 * be _start() itself, written directly in C).
 */
#include <stdlib.h>

int main(int argc, char *argv[]);
extern char **environ;

void _start_c(int argc, char *argv[], char *envp[])
{
    environ = envp;
    exit(main(argc, argv));
}
