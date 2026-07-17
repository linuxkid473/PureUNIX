/* Override for glibc's <sys/utsname.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). uname() is implemented
 * for real in user/newlib_syscalls.c — PureUNIX has no version/hostname
 * syscall, so it reports fixed compile-time strings rather than failing
 * outright (matches what a real embedded system with a frozen kernel
 * version often does).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_UTSNAME_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};

int uname(struct utsname *buf);


#ifdef __cplusplus
}
#endif
#endif
