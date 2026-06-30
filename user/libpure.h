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

/* open() flags */
#define O_RDONLY 0

/* lseek() whence values */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Negative error codes returned by syscalls */
#define ENOENT  (-2)
#define EBADF   (-9)
#define EISDIR  (-21)
#define EINVAL  (-22)
#define EMFILE  (-24)

/* Layout must match struct pureunix_stat in include/pureunix/stat.h. */
struct stat {
    unsigned int   st_size;   /* file size in bytes; 0 for directories */
    unsigned int   st_type;   /* 1 = file, 2 = directory */
    unsigned short st_attr;   /* FAT attribute byte */
};

int    pu_write(int fd, const char *buf, size_t len);
int    pu_read(int fd, char *buf, size_t len);
int    pu_open(const char *path, int flags);
int    pu_close(int fd);
int    pu_lseek(int fd, int offset, int whence);
int    pu_stat(const char *path, struct stat *st);
void   pu_puts(const char *s);
void   pu_puti(int value);
size_t pu_strlen(const char *s);
int    pu_atoi(const char *s);

#endif
