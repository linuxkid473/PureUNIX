/* Override for newlib's own <dirent.h>/<sys/dirent.h> (third_party/newlib/
 * i686-elf/include/sys/dirent.h), which is just the generic "no host
 * support configured" fallback and #errors out unconditionally — newlib
 * was never given a real dirent.h for this bare i686-elf target (see
 * docs/userland.md's newlib section). Found first because
 * $(NEWLIB_CFLAGS) puts -isystem user/newlib_compat ahead of -isystem
 * .../newlib/include (see the Makefile) — same "compat headers shadow the
 * vendored ones" trick user/vi/compat/ uses for Neatvi, just narrower in
 * scope (only dirent.h needs it; everything else about newlib's headers is
 * fine as-is).
 *
 * opendir()/readdir()/closedir() are implemented in user/newlib_syscalls.c
 * over PureUNIX's SYS_READDIR, which has no cursor of its own — it dumps a
 * whole directory into a caller-supplied array in one call. DIR buffers
 * that whole dump and hands out entries one at a time; see that file for
 * struct DIR's real definition (opaque here, same idiom as <stdio.h>'s
 * FILE).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_DIRENT_H
#define PUREUNIX_NEWLIB_COMPAT_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Standard values (matching Linux/glibc) so code that compares d_type
 * against these constants works unmodified. PureUNIX's own SYS_READDIR
 * only distinguishes file/directory/symlink (include/pureunix/dirent.h),
 * so every other DT_* a real target might report is never produced here —
 * callers that fall back to lstat() on DT_UNKNOWN (the usual portable
 * pattern) still work correctly. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10
/* Never actually produced by xlate_dtype() in user/newlib_syscalls.c
 * (SYS_READDIR only ever reports file/directory/symlink) — defined so
 * code that merely switch()es on every standard DT_* value (e.g. GIO's
 * gio/glocalfileenumerator.c) still compiles; matching standard Linux
 * values costs nothing since they're never actually returned here. */
#define DT_FIFO    1
#define DT_CHR     2
#define DT_BLK     6
#define DT_SOCK    12

/* Must match PUREUNIX_MAX_NAME (include/pureunix/config.h) / PU_MAX_NAME
 * (user/libpure.h) — the longest name SYS_READDIR can report. */
#define PU_DIRENT_NAME_MAX 64

struct dirent {
    unsigned int d_type;
    unsigned long d_ino; /* always 0 — SYS_READDIR reports no inode number
                          * at all; real, disclosed limitation (see
                          * readdir()'s own comment in
                          * user/newlib_syscalls.c). Callers that use
                          * d_ino only as a sort/dedup hint (e.g. GIO's
                          * sort_by_inode()) degrade to a harmless no-op,
                          * not a correctness bug. */
    char d_name[PU_DIRENT_NAME_MAX];
};

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);
/* Honest ENOSYS stub — see user/newlib_syscalls.c's real definition for
 * why a real fd-backed dirfd() isn't meaningful on this kernel. */
int dirfd(DIR *dirp);
/* Honest ENOSYS stub (user/newlib_syscalls.c) — SYS_READDIR is path-based
 * only, so there's no way to recover a directory listing from a bare fd,
 * the same real, disclosed limitation as g_unix_fd_query_path() (see
 * third_party/glib/patches/0001-fd-query-path-unsupported-platform.patch).
 * Narrow, real blast radius: only GIO's g_file_measure_disk_usage() (the
 * PCManFM-Qt "folder properties: size" feature) hits this; ordinary
 * directory listing goes through plain opendir(), unaffected. */
DIR *fdopendir(int fd);


#ifdef __cplusplus
}
#endif
#endif
