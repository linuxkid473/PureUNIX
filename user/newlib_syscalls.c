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
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    PU_SYS_MKDIR  = 17,
    PU_SYS_UNLINK = 18,
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

char *__env[1] = { 0 };
char **environ = __env;

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

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

int link(const char *old_path, const char *new_path)
{
    int r = raw_syscall(PU_SYS_LINK, (int)old_path, (int)new_path, 0);
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
    (void)argv; /* SYS_EXEC takes a bare path — no argv/envp support yet. */
    (void)envp;
    int r = raw_syscall(PU_SYS_EXEC, (int)path, 0, 0);
    return fail(r);
}

pid_t wait(int *status)
{
    int r = raw_syscall(PU_SYS_WAIT, -1, (int)status, 0);
    if (r < 0) {
        errno = ECHILD;
        return -1;
    }
    return r;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    int r = raw_syscall(PU_SYS_WAIT, pid, (int)status, 0);
    if (r < 0) {
        errno = ECHILD;
        return -1;
    }
    return r;
}

/* No signal delivery in this kernel yet (docs/syscalls.md). raise()/abort()
 * still need to be able to end the process, so treat "kill yourself" as
 * exit and anything else as ESRCH. */
int kill(pid_t pid, int sig)
{
    if (pid == getpid()) {
        pu_terminate(128 + sig);
    }
    errno = ESRCH;
    return -1;
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
