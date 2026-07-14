#ifndef PUREUNIX_PTY_USER_H
#define PUREUNIX_PTY_USER_H

/* Userspace-facing declaration for SYS_PTY_CREATE (include/pureunix/pty.h,
 * docs/pude.md), implemented in user/newlib_syscalls.c — same "narrow,
 * separate header" pattern as user/pureunix_gfx.h for the SDL2 syscalls.
 * TIOCSWINSZ/TIOCSCTTY (the other two PTY-only pieces of API) are ordinary
 * ioctl() requests, declared in user/newlib_compat/sys/ioctl.h instead —
 * this header is only for the one syscall <sys/ioctl.h>/<unistd.h> has no
 * POSIX name for. */

/* Creates a real PTY pair. fds[0] receives the master fd (PUTerm's end),
 * fds[1] the slave fd (dup2()'d onto the forked shell's stdin/stdout/
 * stderr, exactly like a real terminal emulator's fork()+exec()). Returns
 * 0 on success, or a negative errno (-EMFILE: no free fd slots; -ENOSPC:
 * kernel/pty.c's fixed pty pool is full). */
int pu_pty_create(int fds[2]);

#endif
