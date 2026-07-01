#include "libpure.h"

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* Exposes the raw syscall gate for callers that need to probe a syscall
 * number directly (e.g. an unrecognized number's -1 return, or SYS_EXIT's
 * pass-through return value) rather than through one of the named
 * wrappers below. */
int pu_syscall_raw(int n, int a, int b, int c)
{
    return syscall3(n, a, b, c);
}

int pu_getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

int pu_yield(void)
{
    return syscall3(SYS_YIELD, 0, 0, 0);
}

int pu_write(int fd, const char *buf, size_t len)
{
    return syscall3(SYS_WRITE, fd, (int)buf, (int)len);
}

int pu_read(int fd, char *buf, size_t len)
{
    return syscall3(SYS_READ, fd, (int)buf, (int)len);
}

int pu_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (int)path, flags, 0);
}

int pu_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}

int pu_lseek(int fd, int offset, int whence)
{
    return syscall3(SYS_LSEEK, fd, offset, whence);
}

int pu_stat(const char *path, struct stat *st)
{
    return syscall3(SYS_STAT, (int)path, (int)st, 0);
}

int pu_access(const char *path, int mode)
{
    return syscall3(SYS_ACCESS, (int)path, mode, 0);
}

int pu_chmod(const char *path, mode_t mode)
{
    return syscall3(SYS_CHMOD, (int)path, (int)mode, 0);
}

int pu_chown(const char *path, uid_t uid, gid_t gid)
{
    return syscall3(SYS_CHOWN, (int)path, (int)uid, (int)gid);
}

int pu_readdir(const char *path, struct dirent *entries, int max_entries)
{
    return syscall3(SYS_READDIR, (int)path, (int)entries, max_entries);
}

int pu_debug_setcred(uid_t uid, gid_t gid)
{
    return syscall3(SYS_DEBUG_SETCRED, (int)uid, (int)gid, 0);
}

int pu_lstat(const char *path, struct stat *st)
{
    return syscall3(SYS_LSTAT, (int)path, (int)st, 0);
}

int pu_readlink(const char *path, char *buf, size_t bufsize)
{
    return syscall3(SYS_READLINK, (int)path, (int)buf, (int)bufsize);
}

int pu_mkdir(const char *path)
{
    return syscall3(SYS_MKDIR, (int)path, 0, 0);
}

int pu_unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (int)path, 0, 0);
}

int pu_rmdir(const char *path)
{
    return syscall3(SYS_RMDIR, (int)path, 0, 0);
}

int pu_rename(const char *old_path, const char *new_path)
{
    return syscall3(SYS_RENAME, (int)old_path, (int)new_path, 0);
}

int pu_link(const char *old_path, const char *new_path)
{
    return syscall3(SYS_LINK, (int)old_path, (int)new_path, 0);
}

int pu_symlink(const char *target, const char *path)
{
    return syscall3(SYS_SYMLINK, (int)target, (int)path, 0);
}

int pu_creat(const char *path)
{
    return pu_open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

size_t pu_strlen(const char *s)
{
    size_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

void pu_puts(const char *s)
{
    pu_write(1, s, pu_strlen(s));
}

void pu_puti(int value)
{
    char buf[16];
    int i = 0;
    if (value == 0) {
        pu_puts("0");
        return;
    }
    if (value < 0) {
        pu_puts("-");
        value = -value;
    }
    while (value) {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (i--) {
        pu_write(1, &buf[i], 1);
    }
}

int pu_atoi(const char *s)
{
    int sign = 1;
    int value = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s++ - '0');
    }
    return value * sign;
}
