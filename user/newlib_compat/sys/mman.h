/* Override for glibc's <sys/mman.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no real
 * virtual-memory mapping syscall (no mmap/munmap/brk — see
 * docs/syscalls.md's "Unimplemented Syscalls"; only a static-array sbrk()
 * backs malloc(), see user/newlib_syscalls.c). mmap()/munmap() themselves
 * (also in newlib_syscalls.c) only support the one case BusyBox's dd applet
 * actually needs — MAP_ANON|MAP_PRIVATE with fd == -1, i.e. "just give me a
 * private scratch buffer" — implemented as a thin malloc()/free() wrapper
 * rather than a real page mapping; any other flags/a real fd fail with
 * ENODEV, same as any other syscall PureUNIX doesn't have yet.
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
