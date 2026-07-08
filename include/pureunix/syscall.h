#ifndef PUREUNIX_SYSCALL_H
#define PUREUNIX_SYSCALL_H

enum {
    SYS_EXIT   = 1,
    SYS_WRITE  = 2,
    SYS_READ   = 3,
    SYS_GETPID = 4,
    SYS_YIELD  = 5,
    SYS_OPEN   = 6,
    SYS_CLOSE  = 7,
    SYS_LSEEK  = 8,
    SYS_STAT   = 9,
    SYS_ACCESS = 10,
    SYS_CHMOD  = 11,
    SYS_CHOWN  = 12,
    SYS_READDIR = 13,
    /* Stage 3A test-only hook: sets the *calling task's* uid/gid outright,
     * with no privilege check whatsoever. This is not a real setuid() —
     * there is no login system or privilege model yet to enforce against.
     * It exists solely so user/ext2test.c can exercise owner/group/other
     * permission logic while running as non-root; nothing outside the
     * regression suite should ever call it. See ext2test.c section [22]. */
    SYS_DEBUG_SETCRED = 14,

    /* Stage 4: symlinks, hard links, and a writable EXT2. */
    SYS_READLINK = 15,
    SYS_LSTAT    = 16,
    SYS_MKDIR    = 17,
    SYS_UNLINK   = 18,
    SYS_RMDIR    = 19,
    SYS_RENAME   = 20,
    SYS_LINK     = 21,
    SYS_SYMLINK  = 22,

    /* Per-process address spaces: duplicate (SYS_FORK), replace-in-place
     * (SYS_EXEC), and reap (SYS_WAIT) a process. See docs/syscalls.md. */
    SYS_FORK     = 23,
    SYS_EXEC     = 24,
    SYS_WAIT     = 25,

    /* Terminal control: get/set the console's struct termios. See
     * include/pureunix/termios.h and drivers/tty.c. */
    SYS_TCGETATTR = 26,
    SYS_TCSETATTR = 27,

    /* Device control. Currently supports exactly one request (TIOCGWINSZ)
     * on the console tty — see include/pureunix/ioctl.h. There is no
     * separate isatty() syscall: userspace derives it from whether
     * SYS_TCGETATTR succeeds (see pu_isatty() in user/libpure.c). */
    SYS_IOCTL = 28,

    /* Per-task working directory. See docs/syscalls.md. */
    SYS_CHDIR  = 29,
    SYS_GETCWD = 30,

    /* Blocking sleep, backed by the PIT tick counter (arch/i386/pit.c).
     * See docs/syscalls.md. */
    SYS_NANOSLEEP = 31,

    /* Read-only credential getters — the write side is SYS_DEBUG_SETCRED
     * (test-only, no privilege check) until a real login/setuid model
     * exists. No separate "effective" uid/gid: PureUNIX has no setuid
     * model, so effective always equals real (see user/newlib_syscalls.c's
     * geteuid()/getegid(), which just alias these). See docs/syscalls.md. */
    SYS_GETUID = 32,
    SYS_GETGID = 33,

    /* Sets a file's atime/mtime directly (EBX: path, ECX: atime, EDX:
     * mtime — each a Unix epoch second count, or 0xFFFFFFFF to leave that
     * one unchanged). Real on EXT2 (fs/ext2/mount.c's ext2_utime()); same
     * -EROFS-on-FAT16 story as SYS_CHMOD/SYS_CHOWN. See docs/syscalls.md. */
    SYS_UTIME = 34,

    /* Wall-clock time as a Unix epoch second count (EBX: pointer to a
     * uint32_t to receive it) — a thin userspace-visible wrapper around
     * kernel/time.c's time_now(), which every write-path timestamp
     * already uses internally. No sub-second resolution exists. See
     * docs/syscalls.md. */
    SYS_GETTIMEOFDAY = 35,

    /* pipe()/dup()/dup2() — see include/pureunix/task.h's open_file_t and
     * docs/syscalls.md. */
    SYS_PIPE = 36,
    SYS_DUP  = 37,
    SYS_DUP2 = 38,

    /* Terminates another task with a signal's POSIX default action — see
     * kernel/task.c's task_kill() and docs/syscalls.md. */
    SYS_KILL = 39,

    /* Minimal fcntl(): F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD only — see
     * arch/i386/syscall.c and docs/syscalls.md. */
    SYS_FCNTL = 40,

    /* Truncate an already-open fd to a given length — see arch/i386/syscall.c
     * and docs/syscalls.md. */
    SYS_FTRUNCATE = 41,

    /* Parent task id — see arch/i386/syscall.c and docs/syscalls.md. */
    SYS_GETPPID = 42,

    /* Sends one ICMP echo request and waits for the matching reply — a
     * thin wrapper around net/icmp.c's icmp_ping(), exposing the kernel's
     * already-complete IPv4/ICMP stack to userspace (see user/ping.c).
     * EBX: destination IPv4 address (ip4_addr_t, host byte order --
     *      include/pureunix/inet.h's IP4_ADDR() convention).
     * ECX: timeout in milliseconds.
     * EDX: optional (may be 0/NULL) pointer to a uint32_t that receives
     *      the round-trip time in milliseconds on success.
     * Returns 0 on a received reply, -ETIMEDOUT if none arrived in time.
     * See arch/i386/syscall.c and docs/syscalls.md. */
    SYS_PING = 43,

    /* fstat(2): metadata for an already-open fd, without needing (or
     * re-resolving) a path. EBX: fd. ECX: struct pureunix_stat *.
     * st_size reflects the open file description's *live* in-memory size
     * (f->size), not a possibly-stale on-disk value, matching real
     * fstat() semantics for a writable fd whose data hasn't been flushed
     * yet. Returns 0 or a negative error. See arch/i386/syscall.c and
     * docs/syscalls.md. */
    SYS_FSTAT = 44,
};

#endif
