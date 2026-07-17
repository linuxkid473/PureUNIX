/* Shadow for newlib's own <sys/stat.h> (third_party/newlib) — real header,
 * genuine gap: newlib only declares lstat()/mknod()/UTIME_NOW/UTIME_OMIT
 * for a short list of specific targets it recognizes (__SPU__/__rtems__/
 * __CYGWIN__ — see that header), not a generic bare i686-elf target.
 * PureUNIX has a real SYS_LSTAT (user/newlib_syscalls.c's lstat(), added
 * alongside this header) and a real SYS_UTIME backing utimensat()/
 * utimes() (same file); mknod() has no backing syscall (no device nodes
 * exist), so it's declaration-only — the same "declare so it compiles,
 * even though nothing in the currently-enabled applet set can call it
 * successfully" pattern as user/newlib_compat/sys/mman.h.
 *
 * #include_next finds the next <sys/stat.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_STAT_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <sys/stat.h>

int lstat(const char *__restrict path, struct stat *__restrict buf);
int mknod(const char *path, mode_t mode, dev_t dev);

#ifndef UTIME_NOW
#define UTIME_NOW -2L
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT -1L
#endif


#ifdef __cplusplus
}
#endif
#endif
