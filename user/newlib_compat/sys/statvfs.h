/* Override for glibc's <sys/statvfs.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). BusyBox's df applet
 * (coreutils/df.c) calls statvfs(), not statfs() -- see user/
 * newlib_syscalls.c's statvfs(), backed by the same SYS_STATFS syscall as
 * statfs() (sys/statfs.h).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_STATVFS_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_STATVFS_H

#include <stddef.h>

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif
