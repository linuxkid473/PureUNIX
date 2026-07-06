/* Override for glibc's <sys/statfs.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * statfs()/fstatfs() syscall. Declarations only; nothing in the applets
 * currently enabled in .config calls these (df/mount are not enabled).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_STATFS_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_STATFS_H

#include <stddef.h>

struct statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_namelen;
    long f_frsize;
    long f_spare[5];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
