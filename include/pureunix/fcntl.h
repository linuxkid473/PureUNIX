#ifndef PUREUNIX_FCNTL_H
#define PUREUNIX_FCNTL_H

#define O_RDONLY 0
/* Stage 4: writable file descriptors (see SYS_OPEN in arch/i386/syscall.c).
 * Values are this kernel's own — they don't need to match Linux's. */
#define O_WRONLY 0x001
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
