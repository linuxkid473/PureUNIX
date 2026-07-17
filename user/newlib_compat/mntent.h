/* Override for glibc's <mntent.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). BusyBox's
 * include/platform.h assumes HAVE_MNTENT_H unless it recognizes the
 * target as a specific non-Linux OS (__APPLE__, __FreeBSD__, ...) — since
 * i686-elf matches none of those, it falls through to "assume glibc-like".
 * PureUNIX does have a real mount table (fs/vfs.c's mount-table VFS) that
 * a real setmntent()/getmntent() could eventually be built on, but nothing
 * in the applets currently enabled in .config reads /etc/mtab, so this is
 * declarations-only for now — extend when something needs it for real.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_MNTENT_H
#define PUREUNIX_NEWLIB_COMPAT_MNTENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#define MOUNTED "/etc/mtab"
#define MNTTAB  "/etc/fstab"

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int   mnt_freq;
    int   mnt_passno;
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
int addmntent(FILE *stream, const struct mntent *mnt);
int endmntent(FILE *stream);
char *hasmntopt(const struct mntent *mnt, const char *opt);


#ifdef __cplusplus
}
#endif
#endif
