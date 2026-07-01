#ifndef PUREUNIX_STAT_H
#define PUREUNIX_STAT_H

#include <pureunix/types.h>

/* File-type bits, packed into the high nibble(s) of a 16-bit mode. Values
 * match the standard Unix/ext2 on-disk encoding, so an EXT2 inode's i_mode
 * can be copied into st_mode with no translation. */
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

/* rwx permission bits. */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

/* Layout must match struct stat in user/libpure.h. */
struct pureunix_stat {
    uint32_t st_size;   /* file size in bytes; 0 for directories */
    uint32_t st_type;   /* 1 = file, 2 = directory */
    uint16_t st_attr;   /* FAT attribute byte (legacy) */

    mode_t   st_mode;   /* file-type bits | rwx permission bits */
    uid_t    st_uid;
    gid_t    st_gid;
    nlink_t  st_nlink;
    ino_t    st_ino;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_blocks;   /* 512-byte blocks allocated */
    uint32_t st_blksize;  /* preferred I/O block size */
};

#endif
