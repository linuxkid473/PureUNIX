/* Override for glibc's <sys/mman.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * mmap()/munmap() syscall yet (only a static-array sbrk() — see
 * user/newlib_syscalls.c). Declarations only, so code that doesn't
 * actually call mmap() (true of every applet currently enabled in
 * .config) still compiles; anything that does call it will fail to link,
 * same as any other syscall PureUNIX doesn't have yet.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_MMAN_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);

#endif
