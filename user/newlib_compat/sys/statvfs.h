/* Override for glibc's <sys/statvfs.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). BusyBox's df applet
 * (coreutils/df.c) calls statvfs(), not statfs() -- see user/
 * newlib_syscalls.c's statvfs(), backed by the same SYS_STATFS syscall as
 * statfs() (sys/statfs.h).
 *
 * ST_RDONLY/struct statvfs64/statvfs64(): added for Qt Core's
 * src/corelib/io/qstorageinfo_unix.cpp, which picks struct statvfs64
 * specifically whenever QT_LARGEFILE_SUPPORT is defined (which Qt's own
 * build always does globally) rather than plain struct statvfs - same
 * fields, PureUnix has no real 32-vs-64-bit file-offset distinction to
 * make (see docs/qt-port.md), so statvfs64() is just a thin cast-through
 * to the real statvfs() above.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_STATVFS_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_STATVFS_H

#ifdef __cplusplus
extern "C" {
#endif

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

#ifndef ST_RDONLY
#define ST_RDONLY 1
#endif

struct statvfs64 {
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

int statvfs64(const char *path, struct statvfs64 *buf);


#ifdef __cplusplus
}
#endif
#endif
