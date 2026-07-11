/* Override for glibc's <sys/mount.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). BusyBox's mount/umount
 * applets need mount()/umount()/umount2() and the MS_ and MNT_ flag
 * constants they pass through -- PureUNIX's SYS_MOUNT only ever mounts
 * EXT2 (see include/pureunix/syscall.h), so every flag is accepted but
 * ignored (see user/newlib_syscalls.c's mount()/umount2()).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_MOUNT_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_MOUNT_H

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_REMOUNT     32
#define MS_BIND        4096
#define MS_MGC_VAL     0xC0ED0000
#define MS_MGC_MSK     0xffff0000

#define MNT_FORCE      1
#define MNT_DETACH     2
#define MNT_EXPIRE     4
#define UMOUNT_NOFOLLOW 8

int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#endif
