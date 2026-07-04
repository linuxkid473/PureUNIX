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
};

#endif
