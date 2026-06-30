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
};

#endif
