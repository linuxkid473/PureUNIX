/* Override for glibc's <sys/uio.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). struct iovec/readv/
 * writev are real, general POSIX I/O primitives (not socket-specific),
 * implemented for real in user/newlib_syscalls.c as a loop over the
 * existing read()/write() syscalls.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_UIO_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_UIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif
#endif
