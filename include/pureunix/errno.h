#ifndef PUREUNIX_ERRNO_H
#define PUREUNIX_ERRNO_H

/* Positive error codes; negate when returning from syscalls. */
#define ENOENT   2   /* no such file or directory */
#define EIO      5   /* I/O error */
#define EBADF    9   /* bad file descriptor */
#define EISDIR   21  /* is a directory */
#define EINVAL   22  /* invalid argument */
#define EMFILE   24  /* too many open files */
#define EROFS    30  /* read-only filesystem */
#define ENOSYS   38  /* function not implemented */

#endif
