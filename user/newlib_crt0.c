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
 *
 * .init_array/.fini_array (and .ctors/.dtors — see below): C++ global
 * constructors/destructors. The kernel ELF loader never looks at these
 * specially (see docs/qt-port.md section 2) — they're just ordinary
 * function-pointer data inside the .data PT_LOAD segment (see
 * user/linker.ld), so running them is entirely this file's job.
 * Constructors run in array order before main(); the destructor loop is
 * registered via atexit() (run in reverse array order) so it participates
 * correctly in newlib's normal exit() sequence alongside any user atexit()
 * calls and local-static __cxa_atexit destructors, rather than firing
 * before/after them unconditionally.
 *
 * This i686-elf-gcc 16.1.0 build turns out to emit the legacy .ctors
 * section for ordinary global constructors, not .init_array (verified by
 * inspecting a compiled .o directly — .init_array came up empty even
 * though the program plainly had non-trivial global objects). Both are
 * walked here; whichever one is actually populated does the real work,
 * the other is just an empty array. Order between separate translation
 * units' constructors is unspecified by the C++ standard anyway (only
 * within-TU order is guaranteed, which the compiler already gets right by
 * bundling a TU's constructors into one wrapper function), so a plain
 * forward walk is fully conforming regardless of which convention a given
 * object file used. Destructors for ordinary global objects turn out to
 * always go through __cxa_atexit (registered by the constructor itself, at
 * construction time) rather than a real .dtors/.fini_array entry in
 * practice — this fini walk is defensive/empty in the common case, not
 * dead code removed, since nothing guarantees every possible object file
 * will behave identically.
 */
#include <stdlib.h>

int main(int argc, char *argv[]);
extern char **environ;

/* __cxa_atexit()'s "which shared object" handle. Normally supplied by
 * libgcc's crtbegin.o for the main executable; PureUnix has no crt startup
 * objects at all (see docs/qt-port.md section 1 — no dynamic linking
 * exists, so there is only ever one "module" per process) and no
 * -nostartfiles substitute for it, hence defining it here. Any non-null,
 * stable address works — __cxa_atexit only ever compares it for identity.
 */
void *__dso_handle = &__dso_handle;

typedef void (*init_func_t)(void);
extern init_func_t __init_array_start[];
extern init_func_t __init_array_end[];
extern init_func_t __fini_array_start[];
extern init_func_t __fini_array_end[];
extern init_func_t __ctors_start[];
extern init_func_t __ctors_end[];
extern init_func_t __dtors_start[];
extern init_func_t __dtors_end[];

/* C++ exception unwind table registration (see user/linker.ld's .eh_frame
 * comment for why this is needed at all: PureUnix has no dl_iterate_phdr-
 * style dynamic loader, so libgcc's unwinder only ever finds frame info
 * that was explicitly registered on this list). __register_frame's public
 * contract just wants a pointer to this binary's .eh_frame content (a
 * sequence of CIE/FDE records, terminated by the 4-byte zero length record
 * every GCC-emitted .eh_frame section already ends with) — it allocates
 * its own bookkeeping internally, unlike the lower-level
 * __register_frame_info(). Declared here rather than pulled from a libgcc
 * header since there is no public header for it (it's an internal-but-
 * exported libgcc entry point, same as glibc-less bare-metal ports use).
 */
extern void __register_frame(const void *begin);
extern void __deregister_frame(const void *begin);
extern char __eh_frame_start[];

static void deregister_eh_frame(void)
{
    __deregister_frame(__eh_frame_start);
}

static void run_fini_array(void)
{
    for (init_func_t *f = __fini_array_end; f != __fini_array_start;) {
        (*--f)();
    }
    for (init_func_t *f = __dtors_end; f != __dtors_start;) {
        (*--f)();
    }
}

void _start_c(int argc, char *argv[], char *envp[])
{
    environ = envp;

    __register_frame(__eh_frame_start);
    atexit(deregister_eh_frame);

    for (init_func_t *f = __init_array_start; f != __init_array_end; f++) {
        (*f)();
    }
    for (init_func_t *f = __ctors_start; f != __ctors_end; f++) {
        (*f)();
    }
    atexit(run_fini_array);

    exit(main(argc, argv));
}
