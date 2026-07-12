#ifndef PUREUNIX_ERRNO_H
#define PUREUNIX_ERRNO_H

/* Positive error codes; negate when returning from syscalls. */
#define EPERM    1   /* operation not permitted */
#define ENOENT   2   /* no such file or directory */
#define ESRCH    3   /* no such process (SYS_KILL on a nonexistent pid) */
#define EINTR    4   /* interrupted (VINTR fired mid-read; see drivers/tty.c) */
#define EIO      5   /* I/O error */
#define E2BIG    7   /* argument list too long (SYS_EXEC) */
#define ENOEXEC  8   /* not a valid ELF executable (SYS_EXEC) */
#define EBADF    9   /* bad file descriptor */
#define ENOMEM   12  /* out of memory */
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
#define ERANGE   34  /* result too large for the caller's buffer (getcwd()) */
#define EPIPE    32  /* write() on a pipe with no readers left (SYS_WRITE, SYS_PIPE) */
#define ETIMEDOUT 60 /* SYS_PING got no reply within its timeout */
#define EAGAIN   11  /* resource temporarily unavailable (fcntl F_SETLK contention — see kernel/flock.c) */
#define ENOLCK   37  /* no locks available (fcntl advisory-lock table exhausted) */

#endif
