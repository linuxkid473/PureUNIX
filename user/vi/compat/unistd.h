#ifndef VI_COMPAT_UNISTD_H
#define VI_COMPAT_UNISTD_H

#include <stddef.h>

long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);
int close(int fd);
long lseek(int fd, long offset, int whence);
int unlink(const char *path);
int access(const char *path, int mode);
/* No-op: PureUNIX write-mode fds always start truncated to empty (see
 * arch/i386/syscall.c's SYS_OPEN), so by the time lbuf.c calls this after
 * writing exactly the file's new byte count, the file is already that
 * exact size — there's never stale trailing data to trim. */
int ftruncate(int fd, long length);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#endif
