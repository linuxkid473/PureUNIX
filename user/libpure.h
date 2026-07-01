#ifndef LIBPURE_H
#define LIBPURE_H

#include <stddef.h>
#include <stdint.h>

/* Syscall numbers — must match include/pureunix/syscall.h */
#define SYS_EXIT   1
#define SYS_WRITE  2
#define SYS_READ   3
#define SYS_GETPID 4
#define SYS_YIELD  5
#define SYS_OPEN   6
#define SYS_CLOSE  7
#define SYS_LSEEK  8
#define SYS_STAT   9
#define SYS_ACCESS 10
#define SYS_CHMOD  11
#define SYS_CHOWN  12
#define SYS_READDIR 13
#define SYS_DEBUG_SETCRED 14
#define SYS_READLINK 15
#define SYS_LSTAT    16
#define SYS_MKDIR    17
#define SYS_UNLINK   18
#define SYS_RMDIR    19
#define SYS_RENAME   20
#define SYS_LINK     21
#define SYS_SYMLINK  22

/* open() flags — must match include/pureunix/fcntl.h */
#define O_RDONLY 0
#define O_WRONLY 0x001
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* lseek() whence values */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* access() mode bits — must match include/pureunix/vfs.h */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* Negative error codes returned by syscalls */
#define EPERM   (-1)
#define ENOENT  (-2)
#define EACCES  (-13)
#define EBADF   (-9)
#define EEXIST  (-17)
#define EXDEV   (-18)
#define ENOTDIR (-20)
#define EISDIR  (-21)
#define EINVAL  (-22)
#define EMFILE  (-24)
#define ENOSPC  (-28)
#define EROFS   (-30)
#define ENAMETOOLONG (-36)
#define ENOTEMPTY (-39)
#define ELOOP   (-40)

/* Must match PUREUNIX_MAX_NAME in include/pureunix/config.h. */
#define PU_MAX_NAME 64

typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;

/* Mode bits — must match include/pureunix/stat.h. */
#define S_IFMT   0xF000
#define S_IFSOCK 0xC000
#define S_IFLNK  0xA000
#define S_IFREG  0x8000
#define S_IFBLK  0x6000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

/* Layout must match struct pureunix_stat in include/pureunix/stat.h. */
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = file, 2 = directory, 3 = symlink */
    unsigned short st_attr;   /* FAT attribute byte (legacy) */

    mode_t   st_mode;
    uid_t    st_uid;
    gid_t    st_gid;
    unsigned int st_nlink;
    unsigned int st_ino;
    unsigned int st_atime;
    unsigned int st_mtime;
    unsigned int st_ctime;
    unsigned int st_blocks;
    unsigned int st_blksize;
};

/* Layout must match struct pureunix_dirent in include/pureunix/dirent.h. */
struct dirent {
    char name[PU_MAX_NAME];
    unsigned int type;   /* 1 = file, 2 = directory, 3 = symlink */
    unsigned int size;
};

int    pu_write(int fd, const char *buf, size_t len);
int    pu_read(int fd, char *buf, size_t len);
int    pu_open(const char *path, int flags);
int    pu_close(int fd);
int    pu_lseek(int fd, int offset, int whence);
int    pu_stat(const char *path, struct stat *st);
/* Like pu_stat, but never follows a symlink named by the final path
 * component (ancestor directories are still resolved through any symlinks
 * they contain). */
int    pu_lstat(const char *path, struct stat *st);
int    pu_access(const char *path, int mode);
int    pu_chmod(const char *path, mode_t mode);
int    pu_chown(const char *path, uid_t uid, gid_t gid);
/* Returns the number of entries written into entries[] (up to max_entries),
 * or a negative errno. */
int    pu_readdir(const char *path, struct dirent *entries, int max_entries);
/* Test-only: overrides the calling task's credentials outright, with no
 * privilege check. See SYS_DEBUG_SETCRED in include/pureunix/syscall.h —
 * this exists solely for the ext2test regression suite and must not be
 * used anywhere else. */
int    pu_debug_setcred(uid_t uid, gid_t gid);

/* -------------------------------------------------------------------- */
/* Stage 4: symlinks, hard links, writable EXT2                          */
/* -------------------------------------------------------------------- */

/* Reads the raw target of the symlink at path into buf. Never appends a
 * NUL terminator; truncates to bufsize. Returns the number of bytes copied
 * (>= 0), or a negative errno. Fails with EINVAL if path is not a symlink. */
int    pu_readlink(const char *path, char *buf, size_t bufsize);
int    pu_mkdir(const char *path);
int    pu_unlink(const char *path);
int    pu_rmdir(const char *path);
int    pu_rename(const char *old_path, const char *new_path);
int    pu_link(const char *old_path, const char *new_path);
int    pu_symlink(const char *target, const char *path);
/* Equivalent to open(path, O_WRONLY|O_CREAT|O_TRUNC) — POSIX creat(). */
int    pu_creat(const char *path);
void   pu_puts(const char *s);
void   pu_puti(int value);
size_t pu_strlen(const char *s);
int    pu_atoi(const char *s);

#endif
