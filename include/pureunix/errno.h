#ifndef PUREUNIX_ERRNO_H
#define PUREUNIX_ERRNO_H

/* Positive error codes; negate when returning from syscalls. */
#define EPERM    1   /* operation not permitted */
#define ENOENT   2   /* no such file or directory */
#define EINTR    4   /* interrupted (VINTR fired mid-read; see drivers/tty.c) */
#define EIO      5   /* I/O error */
#define EBADF    9   /* bad file descriptor */
#define ENOTTY   25  /* not a tty (tcgetattr()/tcsetattr() on a non-console fd) */
#define EEXIST   17  /* file already exists */
#define ENOTDIR  20  /* not a directory */
#define EISDIR   21  /* is a directory */
#define EINVAL   22  /* invalid argument */
#define EMFILE   24  /* too many open files */
#define ENOSPC   28  /* no space left on device */
#define EROFS    30  /* read-only filesystem */
#define EXDEV    18  /* cross-device link/rename */
#define ENAMETOOLONG 36 /* path or component too long */
#define ENOSYS   38  /* function not implemented */
#define ENOTEMPTY 39 /* directory not empty */
#define ELOOP    40  /* too many symbolic links encountered */
#define EACCES   13  /* permission denied */

#endif
