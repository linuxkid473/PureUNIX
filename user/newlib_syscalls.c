/* Glue between the vendored newlib (third_party/newlib) and PureUNIX's own
 * syscall ABI (int $0x80 — see docs/syscalls.md and user/libpure.c).
 *
 * newlib's configure.host has no case for the bare i686-elf target, so it
 * never builds its own POSIX-named syscall wrappers (the libc/syscalls
 * directory) — it only *calls* open/read/write/... and expects the OS
 * port to supply them. third_party/newlib was built with
 * -DMISSING_SYSCALL_NAMES, which makes newlib's internal reentrant layer
 * (libc/reent) call exactly those plain POSIX names rather than the
 * underscore-prefixed ones, so that's what's implemented below.
 *
 * This file deliberately does NOT include user/libpure.h: libpure.h's own
 * struct stat/dirent/termios and mode_t/uid_t/gid_t typedefs would collide
 * with newlib's identically-named but differently-laid-out versions from
 * <sys/stat.h>/<sys/types.h> in this same translation unit. Instead this
 * file talks the raw int $0x80 ABI directly, duplicating the small bit of
 * syscall-number/flag-value knowledge libpure.c also has.
 *
 * PureUNIX has no brk/mmap syscall (see docs/syscalls.md's "Unimplemented
 * Syscalls"), so sbrk() bumps a pointer through a static array reserved in
 * this program's own .bss instead of asking the kernel for memory — see
 * NEWLIB_HEAP_SIZE below.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Raw syscall gate — identical to syscall3() in user/libpure.c ---- */

static int raw_syscall(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* Syscall numbers — must match include/pureunix/syscall.h. */
enum {
    PU_SYS_WRITE  = 2,
    PU_SYS_READ   = 3,
    PU_SYS_GETPID = 4,
    PU_SYS_OPEN   = 6,
    PU_SYS_CLOSE  = 7,
    PU_SYS_LSEEK  = 8,
    PU_SYS_STAT   = 9,
    PU_SYS_ACCESS = 10,
    PU_SYS_CHMOD  = 11,
    PU_SYS_CHOWN  = 12,
    PU_SYS_READLINK = 15,
    PU_SYS_SYMLINK  = 22,
    PU_SYS_MKDIR   = 17,
    PU_SYS_UNLINK  = 18,
    PU_SYS_RMDIR   = 19,
    PU_SYS_READDIR = 13,
    PU_SYS_IOCTL   = 28,
    PU_SYS_LSTAT   = 16,
    PU_SYS_NANOSLEEP = 31,
    PU_SYS_GETUID = 32,
    PU_SYS_GETGID = 33,
    PU_SYS_UTIME  = 34,
    PU_SYS_GETTIMEOFDAY = 35,
    PU_SYS_PIPE = 36,
    PU_SYS_DUP  = 37,
    PU_SYS_DUP2 = 38,
    PU_SYS_KILL = 39,
    PU_SYS_CHDIR  = 29,
    PU_SYS_GETCWD = 30,
    PU_SYS_LINK   = 21,
    PU_SYS_FORK   = 23,
    PU_SYS_EXEC   = 24,
    PU_SYS_WAIT   = 25,
};

/* open() flags — must match include/pureunix/fcntl.h. Distinct bit layout
 * from newlib's own O_* (sys/_default_fcntl.h), so open() below translates
 * between the two rather than passing flags through. */
enum {
    PU_O_WRONLY = 0x001,
    PU_O_CREAT  = 0x100,
    PU_O_TRUNC  = 0x200,
    PU_O_APPEND = 0x400,
};

/* Layout must match struct pureunix_stat (include/pureunix/stat.h) /
 * struct stat (user/libpure.h) — kept as a private, differently-named
 * struct here purely to avoid clashing with newlib's <sys/stat.h>. Time
 * fields are named raw_*time rather than st_*time because newlib's
 * <sys/stat.h> #defines st_atime/st_mtime/st_ctime as macros (expanding to
 * st_atim.tv_sec etc), which would otherwise mangle this declaration. */
struct pu_raw_stat {
    uint32_t st_size;
    uint32_t st_type;
    uint16_t st_attr;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_nlink;
    uint32_t st_ino;
    uint32_t raw_atime;
    uint32_t raw_mtime;
    uint32_t raw_ctime;
    uint32_t st_blocks;
    uint32_t st_blksize;
};

/* Terminates the calling process — the same int $0x81 mechanism documented
 * in user/crt0.S and user/libpure.c's pu_exit(); this is not the SYS_EXIT
 * (int $0x80) pass-through. */
static void pu_terminate(int code) __attribute__((noreturn));
static void pu_terminate(int code)
{
    __asm__ volatile("int $0x81" : : "b"(code) : "memory");
    for (;;) {
    }
}

/* The real value is assigned by user/newlib_crt0.c's _start_c() before
 * main() runs, from the envp block kernel/elf.c's build_argv_stack() places
 * on this process's initial stack. NULL here only matters for the sliver of
 * startup code (if any) that runs before _start_c — newlib's own getenv()
 * (libc.a) reads this global directly, no wrapper needed in this file. */
char **environ = 0;

/* PureUNIX's negative syscall-return error codes (include/pureunix/errno.h)
 * are the negation of the standard errno value for every code shared with
 * newlib's <sys/errno.h> *except* these four, which newlib numbers
 * differently. Translate explicitly rather than blindly negating. */
static int xlate_errno(int pu_ret)
{
    switch (pu_ret) {
    case -36: return ENAMETOOLONG; /* pureunix ENAMETOOLONG == -36 */
    case -38: return ENOSYS;       /* pureunix ENOSYS       == -38 */
    case -39: return ENOTEMPTY;    /* pureunix ENOTEMPTY    == -39 */
    case -40: return ELOOP;        /* pureunix ELOOP        == -40 */
    default:  return -pu_ret;
    }
}

static int fail(int pu_ret)
{
    errno = xlate_errno(pu_ret);
    return -1;
}

/* -------------------------------------------------------------------- */
/* File descriptor I/O                                                   */
/* -------------------------------------------------------------------- */

int open(const char *path, int flags, ...)
{
    /* Translate newlib's O_* bit layout to PureUNIX's — the two encodings
     * share no bit positions in common except O_RDONLY == 0. */
    int pu_flags = 0;
    if ((flags & 3) != 0) { /* O_WRONLY (1) or O_RDWR (2) */
        pu_flags |= PU_O_WRONLY;
    }
    if (flags & 0x0008) pu_flags |= PU_O_APPEND; /* newlib O_APPEND */
    if (flags & 0x0200) pu_flags |= PU_O_CREAT;  /* newlib O_CREAT  */
    if (flags & 0x0400) pu_flags |= PU_O_TRUNC;  /* newlib O_TRUNC  */

    int r = raw_syscall(PU_SYS_OPEN, (int)path, pu_flags, 0);
    return r < 0 ? fail(r) : r;
}

int close(int fd)
{
    int r = raw_syscall(PU_SYS_CLOSE, fd, 0, 0);
    return r < 0 ? fail(r) : 0;
}

/* sys/config.h picks _READ_WRITE_RETURN_TYPE == int for this target (no
 * off64_t/large-file support configured), not ssize_t — must match exactly
 * or the declaration in <sys/unistd.h> and this definition conflict. */
int read(int fd, void *buf, size_t len)
{
    int r = raw_syscall(PU_SYS_READ, fd, (int)buf, (int)len);
    return r < 0 ? fail(r) : r;
}

int write(int fd, const void *buf, size_t len)
{
    int r = raw_syscall(PU_SYS_WRITE, fd, (int)buf, (int)len);
    return r < 0 ? fail(r) : r;
}

off_t lseek(int fd, off_t offset, int whence)
{
    int r = raw_syscall(PU_SYS_LSEEK, fd, (int)offset, whence);
    return r < 0 ? fail(r) : r;
}

/* PureUNIX has no fstat(2) — SYS_STAT only takes a path, not an fd (see
 * docs/syscalls.md's "Unimplemented Syscalls"). Descriptors 0-2 report as
 * a character device (matching isatty() below); anything else reports as
 * a plausible-looking regular file so newlib's stdio buffer sizing
 * (__smakebuf, which calls fstat on first write) doesn't fail. */
int fstat(int fd, struct stat *st)
{
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = (fd >= 0 && fd <= 2) ? (S_IFCHR | 0666) : (S_IFREG | 0644);
    st->st_blksize = 1024;
    st->st_nlink = 1;
    return 0;
}

int stat(const char *path, struct stat *st)
{
    struct pu_raw_stat raw;
    int r = raw_syscall(PU_SYS_STAT, (int)path, (int)&raw, 0);
    if (r < 0) {
        return fail(r);
    }
    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_size = raw.st_size;
        st->st_mode = raw.st_mode;
        st->st_uid = raw.st_uid;
        st->st_gid = raw.st_gid;
        st->st_nlink = raw.st_nlink ? raw.st_nlink : 1;
        st->st_ino = raw.st_ino;
        st->st_blocks = raw.st_blocks;
        st->st_blksize = raw.st_blksize ? raw.st_blksize : 1024;
        st->st_atim.tv_sec = raw.raw_atime;
        st->st_mtim.tv_sec = raw.raw_mtime;
        st->st_ctim.tv_sec = raw.raw_ctime;
    }
    return 0;
}

int lstat(const char *path, struct stat *st)
{
    struct pu_raw_stat raw;
    int r = raw_syscall(PU_SYS_LSTAT, (int)path, (int)&raw, 0);
    if (r < 0) {
        return fail(r);
    }
    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_size = raw.st_size;
        st->st_mode = raw.st_mode;
        st->st_uid = raw.st_uid;
        st->st_gid = raw.st_gid;
        st->st_nlink = raw.st_nlink ? raw.st_nlink : 1;
        st->st_ino = raw.st_ino;
        st->st_blocks = raw.st_blocks;
        st->st_blksize = raw.st_blksize ? raw.st_blksize : 1024;
        st->st_atim.tv_sec = raw.raw_atime;
        st->st_mtim.tv_sec = raw.raw_mtime;
        st->st_ctim.tv_sec = raw.raw_ctime;
    }
    return 0;
}

/* PureUNIX has no device nodes (no /dev, no block/char device concept at
 * the VFS layer) — declared only so code that references mknod() at all
 * compiles (see user/newlib_compat/sys/stat.h); any real call fails
 * exactly like it would on a filesystem that doesn't support device
 * nodes. */
int mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)path;
    (void)mode;
    (void)dev;
    errno = ENOSYS;
    return -1;
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

/* struct timespec is layout-compatible with PureUNIX's own
 * struct pureunix_timespec (both {long tv_sec; long tv_nsec;} on i686 —
 * see include/pureunix/time.h), so this passes the pointers straight
 * through with no field-by-field translation, unlike stat()/readdir(). */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    int r = raw_syscall(PU_SYS_NANOSLEEP, (int)req, (int)rem, 0);
    return r < 0 ? fail(r) : 0;
}

/* No SYS_UNAME — PureUNIX has no version/hostname state to report on, so
 * this reports fixed compile-time strings instead of failing outright
 * (the README's own version banner, "PureUnix 0.1.0 for i686" — see
 * kernel/main.c — is the closest thing to a canonical version string). */
/* No env-clearing syscall exists (there's no per-process environment
 * state in the kernel to clear — environ is just a userspace array on
 * this process's own stack, see user/newlib_crt0.c) — clearing it is
 * exactly this simple. */
int clearenv(void)
{
    if (environ) {
        environ[0] = 0;
    }
    return 0;
}

int uname(struct utsname *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    strcpy(buf->sysname, "PureUnix");
    strcpy(buf->nodename, "pureunix");
    strcpy(buf->release, "0.1.0");
    strcpy(buf->version, "PureUnix 0.1.0");
    strcpy(buf->machine, "i686");
    return 0;
}

uid_t getuid(void)
{
    return (uid_t)raw_syscall(PU_SYS_GETUID, 0, 0, 0);
}

gid_t getgid(void)
{
    return (gid_t)raw_syscall(PU_SYS_GETGID, 0, 0, 0);
}

/* PureUNIX has no setuid model — a task's uid/gid never diverge from its
 * "real" values the way a real process's effective ID can via a setuid
 * binary — so effective is always identical to real. */
uid_t geteuid(void)
{
    return getuid();
}

gid_t getegid(void)
{
    return getgid();
}

/* PureUNIX has no supplementary-group concept (task_t carries exactly one
 * uid and one gid — include/pureunix/task.h) — every task belongs to
 * zero supplementary groups. */
int getgroups(int size, gid_t list[])
{
    (void)size;
    (void)list;
    return 0;
}

/* Process umask: purely userspace state (PureUNIX's own SYS_MKDIR/SYS_OPEN
 * don't consult it — see arch/i386/syscall.c's comments on directories/
 * files having no meaningful mode bits to mask yet), but implementing the
 * API contract for real (state persists across calls, old value returned)
 * costs nothing and means anything that reads it back via a second
 * umask() call still sees consistent behavior. */
static mode_t g_umask = 022;

mode_t umask(mode_t mask)
{
    mode_t old = g_umask;
    g_umask = mask & 0777;
    return old;
}

/* lchown(): PureUNIX's SYS_CHOWN has no "don't follow the final symlink"
 * variant (fs/vfs.c's vfs_chown() always resolves through vfs_dispatch()
 * the same way every other path-taking syscall does), so this is simply
 * chown() — only observably different from a real lchown() when path
 * itself names a symlink, which none of the applets currently enabled in
 * .config exercise. */
int lchown(const char *path, uid_t owner, gid_t group)
{
    return chown(path, owner, group);
}

/* dirname(): a pure string operation (POSIX libgen.h) — newlib declares
 * the prototype (third_party/newlib/i686-elf/include/libgen.h) but this
 * vendored build's libc.a has no actual implementation linked in, unlike
 * its sibling basename() (which does). Standard in-place algorithm,
 * matching glibc's actual behavior (strips trailing slashes, then the
 * last component; "/" and "." are special-cased). */
char *dirname(char *path)
{
    static char dot[] = ".";
    if (!path || !*path) {
        return dot;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return dot;
    }
    if (last_slash == path) {
        path[1] = '\0';
        return path;
    }
    *last_slash = '\0';
    return path;
}

/* sleep(): the classic whole-seconds convenience wrapper over nanosleep().
 * Always returns 0 (fully slept) — PureUNIX has no signal delivery to
 * interrupt it early (see nanosleep()'s own comment). */
unsigned int sleep(unsigned int seconds)
{
    struct timespec req;
    req.tv_sec = (long)seconds;
    req.tv_nsec = 0;
    nanosleep(&req, 0);
    return 0;
}

/* Wall-clock seconds since the Unix epoch, straight from SYS_GETTIMEOFDAY
 * (kernel/time.c's time_now(), the same clock every EXT2 write-path
 * timestamp already uses). No sub-second resolution exists, so tv_usec is
 * always 0 and tz is never filled in (matches real gettimeofday()'s own
 * "obsolete, do not use tz" advice). */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) {
        errno = EFAULT;
        return -1;
    }
    uint32_t now = 0;
    int r = raw_syscall(PU_SYS_GETTIMEOFDAY, (int)&now, 0, 0);
    if (r < 0) {
        return fail(r);
    }
    tv->tv_sec = (time_t)now;
    tv->tv_usec = 0;
    return 0;
}

time_t time(time_t *out)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return (time_t)-1;
    }
    if (out) {
        *out = tv.tv_sec;
    }
    return tv.tv_sec;
}

/* Translates POSIX's three time-setting flavors down to SYS_UTIME's plain
 * {path, atime, mtime} — dirfd is always treated as AT_FDCWD-relative
 * (every PureUNIX path syscall already resolves relative to this task's
 * own cwd, so there's no other fd-relative base to honor), and flags'
 * AT_SYMLINK_NOFOLLOW is ignored (SYS_UTIME has no "don't follow the
 * final symlink" variant, same simplification as lchown() above). */
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    (void)dirfd;
    (void)flags;
    uint32_t atime = 0xFFFFFFFFu, mtime = 0xFFFFFFFFu;
    if (!times) {
        atime = mtime = (uint32_t)time(0);
    } else {
        atime = times[0].tv_nsec == UTIME_NOW ? (uint32_t)time(0)
                : times[0].tv_nsec == UTIME_OMIT ? 0xFFFFFFFFu
                : (uint32_t)times[0].tv_sec;
        mtime = times[1].tv_nsec == UTIME_NOW ? (uint32_t)time(0)
                : times[1].tv_nsec == UTIME_OMIT ? 0xFFFFFFFFu
                : (uint32_t)times[1].tv_sec;
    }
    int r = raw_syscall(PU_SYS_UTIME, (int)path, (int)atime, (int)mtime);
    return r < 0 ? fail(r) : 0;
}

/* Older (pre-utimensat) API: struct timeval has no nanosecond/OMIT/NOW
 * sentinel, just seconds+microseconds for both times, or NULL for "now". */
int utimes(const char *path, const struct timeval times[2])
{
    uint32_t atime, mtime;
    if (!times) {
        atime = mtime = (uint32_t)time(0);
    } else {
        atime = (uint32_t)times[0].tv_sec;
        mtime = (uint32_t)times[1].tv_sec;
    }
    int r = raw_syscall(PU_SYS_UTIME, (int)path, (int)atime, (int)mtime);
    return r < 0 ? fail(r) : 0;
}

/* Only TIOCGWINSZ is a real request (SYS_IOCTL, docs/syscalls.md) — any
 * other request fails with -EINVAL at the syscall level, same as it would
 * through libpure's pu_ioctl(). */
int ioctl(int fd, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);
    int r = raw_syscall(PU_SYS_IOCTL, fd, request, (int)argp);
    return r < 0 ? fail(r) : 0;
}

int pipe(int fildes[2])
{
    int r = raw_syscall(PU_SYS_PIPE, (int)fildes, 0, 0);
    return r < 0 ? fail(r) : 0;
}

int dup(int fildes)
{
    int r = raw_syscall(PU_SYS_DUP, fildes, 0, 0);
    return r < 0 ? fail(r) : r;
}

int dup2(int fildes, int fildes2)
{
    int r = raw_syscall(PU_SYS_DUP2, fildes, fildes2, 0);
    return r < 0 ? fail(r) : r;
}

int link(const char *old_path, const char *new_path)
{
    int r = raw_syscall(PU_SYS_LINK, (int)old_path, (int)new_path, 0);
    return r < 0 ? fail(r) : 0;
}

int symlink(const char *target, const char *path)
{
    int r = raw_syscall(PU_SYS_SYMLINK, (int)target, (int)path, 0);
    return r < 0 ? fail(r) : 0;
}

long readlink(const char *path, char *buf, size_t bufsize)
{
    int r = raw_syscall(PU_SYS_READLINK, (int)path, (int)buf, (int)bufsize);
    return r < 0 ? fail(r) : r;
}

/* F_OK/R_OK/W_OK/X_OK (newlib's <unistd.h>) already share PureUNIX's own
 * bit values (0/4/2/1) — see include/pureunix/vfs.h — so mode passes
 * through with no translation. */
int access(const char *path, int mode)
{
    int r = raw_syscall(PU_SYS_ACCESS, (int)path, mode, 0);
    return r < 0 ? fail(r) : 0;
}

int unlink(const char *path)
{
    int r = raw_syscall(PU_SYS_UNLINK, (int)path, 0, 0);
    return r < 0 ? fail(r) : 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode; /* PureUNIX directories have no mode bits yet. */
    int r = raw_syscall(PU_SYS_MKDIR, (int)path, 0, 0);
    return r < 0 ? fail(r) : 0;
}

int rmdir(const char *path)
{
    int r = raw_syscall(PU_SYS_RMDIR, (int)path, 0, 0);
    return r < 0 ? fail(r) : 0;
}

int chmod(const char *path, mode_t mode)
{
    int r = raw_syscall(PU_SYS_CHMOD, (int)path, (int)mode, 0);
    return r < 0 ? fail(r) : 0;
}

/* uid/gid of (uid_t)-1/(gid_t)-1 mean "leave unchanged" (POSIX chown(2)
 * convention) — SYS_CHOWN/ext2_chown() (fs/ext2/mount.c) already implement
 * that, this just passes the sentinel through unchanged. */
int chown(const char *path, uid_t uid, gid_t gid)
{
    int r = raw_syscall(PU_SYS_CHOWN, (int)path, (int)uid, (int)gid);
    return r < 0 ? fail(r) : 0;
}

int chdir(const char *path)
{
    int r = raw_syscall(PU_SYS_CHDIR, (int)path, 0, 0);
    return r < 0 ? fail(r) : 0;
}

char *getcwd(char *buf, size_t size)
{
    int r = raw_syscall(PU_SYS_GETCWD, (int)buf, (int)size, 0);
    return r < 0 ? (fail(r), (char *)0) : buf;
}

/* -------------------------------------------------------------------- */
/* Directory streams (opendir/readdir/closedir)                          */
/* -------------------------------------------------------------------- */

/* SYS_READDIR has no cursor — it dumps up to DIR_CAPACITY entries of a
 * directory into a flat array in one call (include/pureunix/dirent.h).
 * DIR just buffers that whole dump and hands entries out one at a time;
 * opendir() pays the cost of the full readdir() up front rather than
 * pretending to stream. Real definition kept out of <dirent.h>
 * (user/newlib_compat/dirent.h) so DIR stays an opaque handle there, same
 * as <stdio.h>'s FILE. */
#define DIR_CAPACITY 128

struct pu_raw_dirent {
    char name[64]; /* PUREUNIX_MAX_NAME / PU_MAX_NAME */
    uint32_t type; /* 1 = file, 2 = directory, 3 = symlink */
    uint32_t size;
};

struct DIR {
    int count;
    int pos;
    struct dirent current; /* returned by readdir(); overwritten each call,
                             * exactly like every real opendir() implementation */
    struct pu_raw_dirent entries[DIR_CAPACITY];
};

static unsigned int xlate_dtype(uint32_t pu_type)
{
    switch (pu_type) {
    case 1: return DT_REG;
    case 2: return DT_DIR;
    case 3: return DT_LNK;
    default: return DT_UNKNOWN;
    }
}

DIR *opendir(const char *path)
{
    DIR *d = malloc(sizeof(DIR));
    if (!d) {
        errno = ENOMEM;
        return (DIR *)0;
    }
    int n = raw_syscall(PU_SYS_READDIR, (int)path, (int)d->entries, DIR_CAPACITY);
    if (n < 0) {
        free(d);
        fail(n);
        return (DIR *)0;
    }
    d->count = n;
    d->pos = 0;
    return d;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp || dirp->pos >= dirp->count) {
        return (struct dirent *)0;
    }
    struct pu_raw_dirent *src = &dirp->entries[dirp->pos++];
    size_t n = 0;
    while (n < sizeof(dirp->current.d_name) - 1 && src->name[n]) {
        dirp->current.d_name[n] = src->name[n];
        n++;
    }
    dirp->current.d_name[n] = '\0';
    dirp->current.d_type = xlate_dtype(src->type);
    return &dirp->current;
}

int closedir(DIR *dirp)
{
    free(dirp);
    return 0;
}

/* -------------------------------------------------------------------- */
/* Process control                                                       */
/* -------------------------------------------------------------------- */

pid_t getpid(void)
{
    return raw_syscall(PU_SYS_GETPID, 0, 0, 0);
}

pid_t fork(void)
{
    int r = raw_syscall(PU_SYS_FORK, 0, 0, 0);
    if (r < 0) {
        errno = EAGAIN;
        return -1;
    }
    return r;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    /* argv/envp are read out of this process's own memory by the kernel
     * before it switches to the new image's address space (see
     * kernel/elf.c's elf_exec_current()), so passing the raw pointers
     * through is safe even though this call never returns on success. */
    int r = raw_syscall(PU_SYS_EXEC, (int)path, (int)argv, (int)envp);
    return fail(r);
}

/* PureUNIX's SYS_WAIT (docs/syscalls.md) writes the child's bare exit code
 * to *status — that's the documented kernel ABI and what libpure's
 * pu_wait()/user/systest.c's regression coverage both expect verbatim, so
 * it can't change here. But newlib's <sys/wait.h> WIFEXITED/WEXITSTATUS/
 * WIFSIGNALED/WTERMSIG macros expect Linux-style encoding (a normal exit's
 * code in bits 8-15; a signal-terminated death's signal number in the low
 * 7 bits instead). SYS_KILL (docs/syscalls.md) encodes "died from signal
 * N" as a *negative* raw exit code (-N) — never ambiguous with a real exit
 * code, which is always >= 0 — so translating here is just picking which
 * of the two encodings applies. */
static void encode_wait_status(int *status, int raw_code)
{
    if (!status) {
        return;
    }
    if (raw_code < 0) {
        *status = (-raw_code) & 0x7f;
    } else {
        *status = (raw_code & 0xff) << 8;
    }
}

pid_t wait(int *status)
{
    int raw_code = 0;
    int r = raw_syscall(PU_SYS_WAIT, -1, (int)&raw_code, 0);
    if (r < 0) {
        errno = ECHILD;
        return -1;
    }
    encode_wait_status(status, raw_code);
    return r;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    int raw_code = 0;
    int r = raw_syscall(PU_SYS_WAIT, pid, (int)&raw_code, 0);
    if (r < 0) {
        errno = ECHILD;
        return -1;
    }
    encode_wait_status(status, raw_code);
    return r;
}

/* No signal delivery in this kernel yet (docs/syscalls.md). raise()/abort()
 * still need to be able to end the process, so treat "kill yourself" as
 * exit and anything else as ESRCH. */
/* Real now — SYS_KILL (docs/syscalls.md) can terminate *any* task, not just
 * the caller, with a signal's POSIX default action (there's no handler-
 * dispatch mechanism to invoke anything else — see that doc). If pid ==
 * getpid(), the kernel itself never returns from this call (it stops the
 * caller directly, same as _exit()); this only ever returns for some other
 * pid. */
int kill(pid_t pid, int sig)
{
    int r = raw_syscall(PU_SYS_KILL, pid, sig, 0);
    return r < 0 ? fail(r) : 0;
}

void _exit(int code)
{
    pu_terminate(code);
}

/* -------------------------------------------------------------------- */
/* Heap                                                                   */
/* -------------------------------------------------------------------- */

/* The per-process window is 3 MiB minus a 64 KiB stack (USER_WINDOW_BASE/
 * END in include/pureunix/vmm.h); 1 MiB for the heap comfortably leaves
 * the rest for code/data/bss. */
#define NEWLIB_HEAP_SIZE (1 * 1024 * 1024)
static char newlib_heap[NEWLIB_HEAP_SIZE];
static size_t newlib_heap_used = 0;

void *sbrk(ptrdiff_t incr)
{
    if (incr < 0 && (size_t)(-incr) > newlib_heap_used) {
        errno = EINVAL;
        return (void *)-1;
    }
    if (incr > 0 && newlib_heap_used + (size_t)incr > NEWLIB_HEAP_SIZE) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *prev = newlib_heap + newlib_heap_used;
    newlib_heap_used += incr;
    return prev;
}
