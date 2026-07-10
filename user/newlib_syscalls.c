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
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
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
    PU_SYS_YIELD  = 5,
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
    PU_SYS_TCGETATTR = 26,
    PU_SYS_TCSETATTR = 27,
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
    PU_SYS_FCNTL = 40,
    PU_SYS_FTRUNCATE = 41,
    PU_SYS_GETPPID = 42,
    PU_SYS_CHDIR  = 29,
    PU_SYS_GETCWD = 30,
    PU_SYS_LINK   = 21,
    PU_SYS_FORK   = 23,
    PU_SYS_EXEC   = 24,
    PU_SYS_WAIT   = 25,
    PU_SYS_FSTAT  = 44,
    PU_SYS_SETPGID = 45,
    PU_SYS_GETPGID = 46,
    PU_SYS_SETSID  = 47,
    PU_SYS_GETSID  = 48,
    PU_SYS_SIGACTION   = 49,
    PU_SYS_SIGPROCMASK = 50,
    PU_SYS_SIGPENDING  = 51,
    PU_SYS_SETPRIORITY = 52,
    PU_SYS_GETPRIORITY = 53,
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

/* Mirrors include/pureunix/termios.h's struct termios (NCCS == 8, its own
 * small field/bit layout) — kept as a private, differently-named struct
 * here for the same reason as struct pu_raw_stat above: this file can't
 * include pureunix/termios.h directly without colliding with the Linux-
 * shaped struct termios user/newlib_compat/sys/termios.h already declares
 * for this translation unit. */
struct pu_raw_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[8];
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

int chmod(const char *path, mode_t mode);
int access(const char *path, int mode);

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

    /* PureUNIX's raw SYS_OPEN has no mode argument at all (docs/syscalls.md
     * -- just path + flags), unlike POSIX open(path, O_CREAT, mode): every
     * file vfs_create() makes gets a fixed default mode regardless of what
     * a caller asked for. That's a real, user-visible gap -- e.g. TCC
     * (third_party/tcc/) creates its output executable via exactly
     * open(path, O_CREAT|O_TRUNC|O_WRONLY, 0777) and relies on that mode
     * actually taking effect, or the ELF it just wrote can't be exec()'d
     * (elf_exec() requires X_OK -- see kernel/elf.c). Rather than extend
     * the syscall ABI (a 4th register/argument on every SYS_OPEN caller,
     * including every existing one that never needed it), reuse the
     * already-real SYS_CHMOD right after a successful create -- slightly
     * non-atomic (the file briefly exists with vfs_create()'s own default
     * mode) but PureUNIX has no concurrent-access model where that
     * distinction is observable, and every other narrow libc translation
     * in this file (mmap, mprotect, ...) makes the same kind of
     * best-effort-instead-of-a-kernel-change call.
     *
     * Only applied when the file didn't already exist -- POSIX only
     * honors `mode` for an open() call that actually creates the file;
     * O_CREAT on an existing file (no O_EXCL support here either) must
     * leave its permissions untouched, or e.g. `touch` on an existing
     * 0600 file would silently reset it to 0666. */
    mode_t mode = 0;
    int existed_before = 1;
    if (pu_flags & PU_O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        existed_before = (access(path, 0 /* F_OK */) == 0);
    }

    int r = raw_syscall(PU_SYS_OPEN, (int)path, pu_flags, 0);
    if (r >= 0 && (pu_flags & PU_O_CREAT) && !existed_before) {
        chmod(path, mode);
    }
    return r < 0 ? fail(r) : r;
}

int close(int fd)
{
    int r = raw_syscall(PU_SYS_CLOSE, fd, 0, 0);
    return r < 0 ? fail(r) : 0;
}

/* Minimal fcntl(): F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL only — see
 * SYS_FCNTL (arch/i386/syscall.c) and docs/syscalls.md. F_GETFD/F_SETFD are
 * honest no-ops (no close-on-exec model — every fd is always inherited
 * across exec, see task_create_user()/task_fork()). F_GETFL/F_SETFL
 * translate PureUNIX's O_* bit layout to/from newlib's, same as open(). */
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    if (cmd == F_SETFL) {
        int pu_flags = 0;
        if ((arg & 3) != 0) pu_flags |= PU_O_WRONLY;
        if (arg & 0x0008) pu_flags |= PU_O_APPEND;
        if (arg & 0x0200) pu_flags |= PU_O_CREAT;
        if (arg & 0x0400) pu_flags |= PU_O_TRUNC;
        arg = pu_flags;
    }

    int r = raw_syscall(PU_SYS_FCNTL, fd, cmd, arg);
    if (r < 0) {
        return fail(r);
    }
    if (cmd == F_GETFL) {
        int nl_flags = 0;
        if (r & PU_O_WRONLY) nl_flags |= 1;
        if (r & PU_O_APPEND) nl_flags |= 0x0008;
        if (r & PU_O_CREAT)  nl_flags |= 0x0200;
        if (r & PU_O_TRUNC)  nl_flags |= 0x0400;
        return nl_flags;
    }
    return r;
}

int ftruncate(int fd, off_t length)
{
    int r = raw_syscall(PU_SYS_FTRUNCATE, fd, (int)length, 0);
    return r < 0 ? fail(r) : 0;
}

/* Every write() already lands directly in the VFS's in-memory copy of the
 * file (see open_file_t in include/pureunix/task.h) with nothing buffered
 * behind it to flush — a real no-op, not a stub standing in for a missing
 * feature. */
int fsync(int fd)
{
    (void)fd;
    return 0;
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
 * docs/syscalls.md — SYS_FSTAT). Descriptors 0-2 report as a character
 * device (matching isatty() below); any other fd is a real kernel query
 * (SYS_FSTAT) against that fd's open file description, returning its
 * *live* size — not a fabricated st_size=0.
 *
 * This used to be a pure-userspace stub that always reported st_size=0
 * for every non-console fd, with no kernel round-trip at all. That broke
 * any caller that sizes a read buffer from fstat() before reading — e.g.
 * BusyBox ash's own script-file interpreter (`sh script.sh`) treated
 * every script as a 0-byte file and mis-parsed it (observed as a bogus
 * "sh: 3: m" parse error immediately after a successful, correctly-sized
 * open() -- see docs/tcc-port.md's neighboring incompatibility writeups
 * for the same style of "narrow libc stub breaks a real caller" bug). */
int fstat(int fd, struct stat *st)
{
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR | 0666;
        st->st_blksize = 1024;
        st->st_nlink = 1;
        return 0;
    }
    struct pu_raw_stat raw;
    int r = raw_syscall(PU_SYS_FSTAT, fd, (int)&raw, 0);
    if (r < 0) {
        return fail(r);
    }
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

/* One console, no /dev tree — "/dev/console" is as real a name as this
 * kernel has for it. */
int ttyname_r(int fd, char *buf, size_t buflen)
{
    if (!isatty(fd)) {
        return ENOTTY;
    }
    if (!buf || buflen < sizeof("/dev/console")) {
        return ERANGE;
    }
    strcpy(buf, "/dev/console");
    return 0;
}

char *ttyname(int fd)
{
    static char buf[32];
    int r = ttyname_r(fd, buf, sizeof(buf));
    if (r != 0) {
        errno = r;
        return NULL;
    }
    return buf;
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

/* usleep(3): microsecond sleep, built directly on the real nanosleep()
 * above — newlib declares it (sys/unistd.h) but never defines it for
 * this freestanding target. BusyBox's `top` (M8, once CONFIG_TOP is on)
 * calls this for its own refresh interval. */
int usleep(useconds_t usec)
{
    struct timespec req = {
        .tv_sec = (time_t)(usec / 1000000u),
        .tv_nsec = (long)((usec % 1000000u) * 1000u),
    };
    return nanosleep(&req, NULL);
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

/* realpath(): standard POSIX canonicalization (resolve "." / ".." / repeated
 * slashes / symlinks against an absolute base). newlib declares the
 * prototype (stdlib.h) but this vendored libc.a has no implementation, the
 * same gap dirname() above already had. Requires every component *including
 * the last* to exist, matching glibc's own realpath() — BusyBox's own
 * libbb wrapper (xreadlink.c's xmalloc_realpath_coreutils, unmodified
 * upstream) already implements the GNU/coreutils "last component need not
 * exist" leniency itself by retrying against the parent directory when this
 * returns ENOENT, so this function doesn't need to duplicate that. */
#define REALPATH_MAX_SYMLINKS 32

char *realpath(const char *restrict path, char *restrict resolved)
{
    if (!path || !*path) {
        errno = ENOENT;
        return NULL;
    }

    char result[PATH_MAX];
    size_t result_len;

    if (path[0] == '/') {
        result[0] = '/';
        result_len = 1;
        path++;
    } else {
        if (!getcwd(result, sizeof(result))) {
            return NULL; /* errno already set by getcwd() */
        }
        result_len = strlen(result);
    }

    /* `remaining` holds every path component not yet folded into `result`,
     * slash-separated with no leading slash. Symlink targets are spliced in
     * here (in front of whatever was left) so they get resolved in the
     * right order without recursion. */
    char remaining[PATH_MAX];
    if (strlen(path) >= sizeof(remaining)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(remaining, path);

    int symlink_count = 0;
    char *rp = remaining;

    while (*rp) {
        char *slash = strchr(rp, '/');
        size_t complen = slash ? (size_t)(slash - rp) : strlen(rp);
        char *next = slash ? slash + 1 : rp + complen;

        if (complen == 0 || (complen == 1 && rp[0] == '.')) {
            /* empty (repeated slash) or "." - nothing to do */
        } else if (complen == 2 && rp[0] == '.' && rp[1] == '.') {
            if (result_len > 1) {
                while (result_len > 1 && result[result_len - 1] != '/') {
                    result_len--;
                }
                if (result_len > 1) {
                    result_len--; /* drop the '/' too, unless it's root */
                }
            }
        } else {
            if (result_len + 1 + complen >= sizeof(result)) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            char candidate[PATH_MAX];
            memcpy(candidate, result, result_len);
            size_t clen = result_len;
            if (clen == 0 || candidate[clen - 1] != '/') {
                candidate[clen++] = '/';
            }
            memcpy(candidate + clen, rp, complen);
            clen += complen;
            candidate[clen] = '\0';

            struct stat st;
            if (lstat(candidate, &st) < 0) {
                return NULL; /* errno already set by lstat() */
            }

            if (S_ISLNK(st.st_mode)) {
                if (++symlink_count > REALPATH_MAX_SYMLINKS) {
                    errno = ELOOP;
                    return NULL;
                }
                char target[PATH_MAX];
                long tlen = readlink(candidate, target, sizeof(target) - 1);
                if (tlen < 0) {
                    return NULL;
                }
                target[tlen] = '\0';

                size_t rest_len = strlen(next);
                size_t target_len = (size_t)tlen;
                if (target_len + 1 + rest_len >= sizeof(remaining)) {
                    errno = ENAMETOOLONG;
                    return NULL;
                }
                char newremaining[PATH_MAX];
                memcpy(newremaining, target, target_len);
                if (rest_len > 0) {
                    newremaining[target_len] = '/';
                    memcpy(newremaining + target_len + 1, next, rest_len + 1);
                } else {
                    newremaining[target_len] = '\0';
                }
                memcpy(remaining, newremaining, target_len + 1 + rest_len + 1);

                if (target[0] == '/') {
                    result[0] = '/';
                    result_len = 1;
                }
                rp = remaining;
                continue;
            }

            memcpy(result + result_len, candidate + result_len,
                   clen - result_len + 1);
            result_len = clen;
        }

        rp = next;
    }

    if (result_len == 0) {
        result[0] = '/';
        result_len = 1;
    }
    result[result_len] = '\0';

    char *out = resolved ? resolved : malloc(PATH_MAX);
    if (!out) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(out, result, result_len + 1);
    return out;
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

/* TIOCGWINSZ/TIOCSFONT/VT_GETACTIVE/VT_ACTIVATE/TIOCGPGRP/TIOCSPGRP are
 * real requests (SYS_IOCTL, docs/syscalls.md) — any other request fails
 * with -EINVAL at the syscall level, same as it would through libpure's
 * pu_ioctl(). */
int ioctl(int fd, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);
    int r = raw_syscall(PU_SYS_IOCTL, fd, request, (int)argp);
    return r < 0 ? fail(r) : 0;
}

/* tcgetpgrp()/tcsetpgrp() — POSIX's controlling-terminal foreground
 * process group accessors, built on TIOCGPGRP/TIOCSPGRP the same way real
 * libc implementations are. What BusyBox ash's job control (once
 * CONFIG_ASH_JOB_CONTROL is on) uses to move a job into/out of the
 * foreground. See kernel/vt.c's vt_get_fg_pgid()/vt_set_fg_pgid(). */
pid_t tcgetpgrp(int fd)
{
    int pgrp = 0;
    int r = raw_syscall(PU_SYS_IOCTL, fd, TIOCGPGRP, (int)&pgrp);
    return r < 0 ? fail(r) : pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    int r = raw_syscall(PU_SYS_IOCTL, fd, TIOCSPGRP, (int)&pgrp);
    return r < 0 ? fail(r) : 0;
}

/* Real, not stubbed — PureUNIX already has a working termios syscall pair
 * (SYS_TCGETATTR/SYS_TCSETATTR, used by libpure's pu_tcgetattr()/
 * pu_tcsetattr() — e.g. user/vi/'s raw-mode terminal handling). This is
 * just the newlib-tier translation between its own small internal
 * termios layout (include/pureunix/termios.h, mirrored here as
 * struct pu_raw_termios) and the Linux-shaped one BusyBox expects
 * (user/newlib_compat/sys/termios.h) — same kind of bit-by-bit translation
 * open() already does for O_* flags. c_iflag/c_oflag/c_cflag carry no
 * meaningful bits on either side (PureUNIX has one console, no real serial
 * line), so those round-trip as approximations, not exact translations. */
enum {
    PU_ISIG = 0x0001, PU_ICANON = 0x0002, PU_ECHO = 0x0004,
    PU_ECHOE = 0x0008, PU_ECHOK = 0x0010, PU_ECHONL = 0x0020,
};
enum { PU_VINTR = 0, PU_VQUIT = 1, PU_VERASE = 2, PU_VKILL = 3, PU_VEOF = 4, PU_VMIN = 5, PU_VTIME = 6, PU_VSUSP = 7 };

int tcgetattr(int fd, struct termios *out)
{
    struct pu_raw_termios raw;
    int r = raw_syscall(PU_SYS_TCGETATTR, fd, (int)&raw, 0);
    if (r < 0) {
        return fail(r);
    }
    memset(out, 0, sizeof(*out));
    out->c_cflag = CS8 | CREAD | CLOCAL;
    out->c_ispeed = out->c_ospeed = B38400;
    if (raw.c_lflag & PU_ISIG)   out->c_lflag |= ISIG;
    if (raw.c_lflag & PU_ICANON) out->c_lflag |= ICANON;
    if (raw.c_lflag & PU_ECHO)   out->c_lflag |= ECHO;
    if (raw.c_lflag & PU_ECHOE)  out->c_lflag |= ECHOE;
    if (raw.c_lflag & PU_ECHOK)  out->c_lflag |= ECHOK;
    if (raw.c_lflag & PU_ECHONL) out->c_lflag |= ECHONL;
    out->c_cc[VINTR]  = raw.c_cc[PU_VINTR];
    out->c_cc[VQUIT]  = raw.c_cc[PU_VQUIT];
    out->c_cc[VERASE] = raw.c_cc[PU_VERASE];
    out->c_cc[VKILL]  = raw.c_cc[PU_VKILL];
    out->c_cc[VEOF]   = raw.c_cc[PU_VEOF];
    out->c_cc[VMIN]   = raw.c_cc[PU_VMIN];
    out->c_cc[VTIME]  = raw.c_cc[PU_VTIME];
    out->c_cc[VSUSP]  = raw.c_cc[PU_VSUSP];
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *in)
{
    struct pu_raw_termios raw;
    memset(&raw, 0, sizeof(raw));
    if (in->c_lflag & ISIG)   raw.c_lflag |= PU_ISIG;
    if (in->c_lflag & ICANON) raw.c_lflag |= PU_ICANON;
    if (in->c_lflag & ECHO)   raw.c_lflag |= PU_ECHO;
    if (in->c_lflag & ECHOE)  raw.c_lflag |= PU_ECHOE;
    if (in->c_lflag & ECHOK)  raw.c_lflag |= PU_ECHOK;
    if (in->c_lflag & ECHONL) raw.c_lflag |= PU_ECHONL;
    raw.c_cc[PU_VINTR]  = in->c_cc[VINTR];
    raw.c_cc[PU_VQUIT]  = in->c_cc[VQUIT];
    raw.c_cc[PU_VERASE] = in->c_cc[VERASE];
    raw.c_cc[PU_VKILL]  = in->c_cc[VKILL];
    raw.c_cc[PU_VEOF]   = in->c_cc[VEOF];
    raw.c_cc[PU_VMIN]   = in->c_cc[VMIN];
    raw.c_cc[PU_VTIME]  = in->c_cc[VTIME];
    raw.c_cc[PU_VSUSP]  = in->c_cc[VSUSP];
    int r = raw_syscall(PU_SYS_TCSETATTR, fd, (int)&raw, actions);
    return r < 0 ? fail(r) : 0;
}

speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t speed) { t->c_ispeed = speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { t->c_ospeed = speed; return 0; }

void cfmakeraw(struct termios *t)
{
    t->c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

/* No pending output/input queue to drain or flush (see this kernel's
 * SYS_TCSETATTR doc comment, docs/syscalls.md) — every "actions" value
 * already applies immediately. */
int tcflush(int fd, int queue_selector) { (void)fd; (void)queue_selector; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }
int tcflow(int fd, int action) { (void)fd; (void)action; return 0; }
int tcsendbreak(int fd, int duration) { (void)fd; (void)duration; return 0; }

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
    /* glibc/GNU extension: buf == NULL means "malloc a buffer as big as
     * necessary yourself" — ash's own getpwd() (shell/ash.c) relies on
     * exactly this ("huh, using glibc extension?", its own comment says)
     * to seed $PWD/curdir once at shell startup, before any cd. Growing
     * the guess on ERANGE the same way user/systest.c already exercises
     * for a caller-supplied buffer. */
    if (!buf) {
        size_t cap = size ? size : 128;
        for (;;) {
            char *tmp = malloc(cap);
            if (!tmp) {
                errno = ENOMEM;
                return NULL;
            }
            int r = raw_syscall(PU_SYS_GETCWD, (int)tmp, (int)cap, 0);
            if (r == 0) {
                return tmp;
            }
            free(tmp);
            if (xlate_errno(r) != ERANGE) {
                fail(r);
                return NULL;
            }
            cap *= 2;
        }
    }
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

/* Real, not a stub — this is exactly SYS_YIELD, the same cooperative
 * reschedule task_yield() (kernel/task.c) already performs everywhere else
 * in this kernel (e.g. blocking pipe/wait loops). BusyBox's `less` calls
 * this while polling for more input to read. */
int sched_yield(void)
{
    raw_syscall(PU_SYS_YIELD, 0, 0, 0);
    return 0;
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

/* True vfork() shares the parent's address space until exec()/_exit() so a
 * child can replace its image without a real copy — purely a performance
 * optimization on systems where fork() would otherwise copy a large address
 * space. PureUNIX's fork() (task_fork(), kernel/task.c) already gives every
 * child its own real page directory cheaply, so there's nothing vfork()
 * would save here; aliasing it straight to fork() is fully correct, just
 * not the same performance shortcut a hosted OS gives it. */
pid_t vfork(void)
{
    return fork();
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/')) {
        return execve(file, argv, environ);
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/bin";
    }
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char candidate[256];
        if (len > 0 && len + 1 + strlen(file) + 1 <= sizeof(candidate)) {
            memcpy(candidate, p, len);
            candidate[len] = '/';
            strcpy(candidate + len + 1, file);
            execve(candidate, argv, environ);
            if (errno != ENOENT) {
                return -1;
            }
        }
        p = colon ? colon + 1 : p + len;
    }
    errno = ENOENT;
    return -1;
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
 * of the two encodings applies. A third case, raw_code <= -1000, means
 * "stopped by signal -(raw_code+1000)" (kernel/task.c's task_waitpid(),
 * only ever produced when WUNTRACED is passed) — translated to newlib's
 * WIFSTOPPED/WSTOPSIG encoding (stop signal in bits 8-15, low byte 0x7f). */
static void encode_wait_status(int *status, int raw_code)
{
    if (!status) {
        return;
    }
    if (raw_code <= -1000) {
        int stop_sig = -(raw_code + 1000);
        *status = ((stop_sig & 0xff) << 8) | 0x7f;
    } else if (raw_code < 0) {
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
    int raw_code = 0;
    int r = raw_syscall(PU_SYS_WAIT, pid, (int)&raw_code, options);
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

/* killpg(pgrp, sig) is exactly kill(-pgrp, sig) — POSIX's own definition —
 * except pgrp == 0 means "the caller's own process group", which the
 * kernel's SYS_KILL already treats pid == 0 as (docs/syscalls.md), so no
 * translation is needed for that case either. Needed for BusyBox ash's
 * job control (M9): `kill %1` / stopping or backgrounding a whole
 * pipeline signals every process in the job's group at once. */
int killpg(pid_t pgrp, int sig)
{
    if (pgrp < 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(pgrp == 0 ? 0 : -pgrp, sig);
}

pid_t getppid(void)
{
    return raw_syscall(PU_SYS_GETPPID, 0, 0, 0);
}

/* Process groups and sessions — see SYS_SETPGID/SYS_GETPGID/SYS_SETSID/
 * SYS_GETSID in docs/syscalls.md. */
int setpgid(pid_t pid, pid_t pgid)
{
    int r = raw_syscall(PU_SYS_SETPGID, pid, pgid, 0);
    return r < 0 ? fail(r) : 0;
}

pid_t getpgid(pid_t pid)
{
    int r = raw_syscall(PU_SYS_GETPGID, pid, 0, 0);
    return r < 0 ? fail(r) : r;
}

pid_t getpgrp(void)
{
    return getpgid(0);
}

pid_t setsid(void)
{
    int r = raw_syscall(PU_SYS_SETSID, 0, 0, 0);
    return r < 0 ? fail(r) : r;
}

pid_t getsid(pid_t pid)
{
    int r = raw_syscall(PU_SYS_GETSID, pid, 0, 0);
    return r < 0 ? fail(r) : r;
}

/* Real signal delivery — SYS_SIGACTION/SYS_SIGPROCMASK (docs/syscalls.md,
 * kernel/signal.c, arch/i386/signal.c). This layer's only job is
 * translating between newlib's own struct sigaction (sa_handler/sa_mask/
 * sa_flags) and the kernel's much smaller pu_sigaction_t (just a
 * handler) — sa_mask and every sa_flags bit are accepted (so callers that
 * set them don't get an error) but not actually honored by the kernel;
 * see include/pureunix/signal.h's own scope-notes comment for why
 * (SA_NODEFER-equivalent nesting, additional-signals-blocked-during-
 * handler beyond the signal itself, etc. are all out of scope for this
 * kernel's deliberately narrow, boundary-only delivery mechanism). This
 * is honest, not a silent stub: the handler genuinely does run now. */
typedef struct pu_sigaction {
    unsigned int handler;
} pu_sigaction_t;

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    pu_sigaction_t in, out;
    pu_sigaction_t *inp = NULL;
    if (act) {
        in.handler = (unsigned int)(uintptr_t)act->sa_handler;
        inp = &in;
    }
    int r = raw_syscall(PU_SYS_SIGACTION, sig, (int)inp, (int)&out);
    if (r < 0) {
        return fail(r);
    }
    if (oldact) {
        memset(oldact, 0, sizeof(*oldact));
        oldact->sa_handler = (_sig_func_ptr)(uintptr_t)out.handler;
    }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    unsigned int in = set ? (unsigned int)*set : 0;
    unsigned int out = 0;
    int r = raw_syscall(PU_SYS_SIGPROCMASK, how, set ? (int)&in : 0, (int)&out);
    if (r < 0) {
        return fail(r);
    }
    if (oldset) {
        *oldset = out;
    }
    return 0;
}

int sigpending(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    unsigned int out = 0;
    int r = raw_syscall(PU_SYS_SIGPENDING, (int)&out, 0, 0);
    if (r < 0) {
        return fail(r);
    }
    *set = out;
    return 0;
}

/* nice()/renice() — SYS_SETPRIORITY/SYS_GETPRIORITY (docs/syscalls.md)
 * only ever target a single process (there's no real notion of "every
 * process in this group/owned by this user" to iterate — see
 * user/newlib_compat/sys/resource.h's own comment), matching PRIO_PROCESS.
 * PRIO_PGRP/PRIO_USER are rejected here rather than silently
 * misinterpreted. */
int getpriority(int which, id_t who)
{
    if (which != PRIO_PROCESS) {
        errno = EINVAL;
        return -1;
    }
    int out = 0;
    int r = raw_syscall(PU_SYS_GETPRIORITY, (int)who, (int)&out, 0);
    if (r < 0) {
        return fail(r);
    }
    return out;
}

int setpriority(int which, id_t who, int prio)
{
    if (which != PRIO_PROCESS) {
        errno = EINVAL;
        return -1;
    }
    int r = raw_syscall(PU_SYS_SETPRIORITY, (int)who, prio, 0);
    return r < 0 ? fail(r) : 0;
}

/* nice(2): adjusts the caller's own niceness by `incr` relative to its
 * *current* value (not an absolute set like setpriority()) and returns
 * the new value — or -1 with errno set. Real callers distinguish a
 * legitimate result of -1 from an error by clearing errno first, exactly
 * like getpriority() above; this implementation follows the same
 * convention (only sets errno on a genuine failure). */
int nice(int incr)
{
    errno = 0;
    int current = getpriority(PRIO_PROCESS, 0);
    if (current == -1 && errno != 0) {
        return -1;
    }
    int wanted = current + incr;
    if (wanted < -20) {
        wanted = -20;
    } else if (wanted > 19) {
        wanted = 19;
    }
    if (setpriority(PRIO_PROCESS, 0, wanted) != 0) {
        return -1;
    }
    return wanted;
}

/* Real signal delivery would block the caller until one of the unblocked
 * signals in `set` actually arrives; since nothing here ever delivers a
 * signal asynchronously, the closest honest behavior is to yield once (so
 * a cooperative sibling task — e.g. a child ash's dowait() is waiting on —
 * actually gets to run) and report "interrupted", which is exactly the
 * return value/errno a real sigsuspend() gives once any signal arrives. */
int sigsuspend(const sigset_t *set)
{
    (void)set;
    raw_syscall(PU_SYS_YIELD, 0, 0, 0);
    errno = EINTR;
    return -1;
}

/* No per-process CPU-time accounting exists in this kernel's scheduler
 * (cooperative round-robin, docs/scheduler.md) — utime/stime/cutime/cstime
 * are always 0, an honest "not tracked" rather than a fabricated value.
 * The return value (elapsed real time) comes from the same wall-clock
 * SYS_GETTIMEOFDAY backing gettimeofday()/time() above. */
clock_t times(struct tms *buf)
{
    if (buf) {
        memset(buf, 0, sizeof(*buf));
    }
    struct timeval tv = { 0, 0 };
    gettimeofday(&tv, 0);
    return (clock_t)((long)tv.tv_sec * 100 + tv.tv_usec / 10000);
}

/* No select()/poll()-equivalent readiness multiplexing exists in this
 * kernel — every requested fd is reported ready for whatever it asked
 * about immediately. This is enough for the only reachable callers in the
 * currently enabled applet set (ash's `read -t`/`read -s` builtin,
 * libbb's safe_poll() retry wrapper): the real read()/write() call that
 * follows is what actually blocks or returns data, same as it always
 * would — `read -t TIMEOUT` just won't genuinely time out yet. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)timeout;
    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents) {
            ready++;
        }
    }
    return ready;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_CLK_TCK: return 100; /* matches pit_init(100), kernel/main.c */
    case _SC_OPEN_MAX: return 32; /* MAX_OPEN_FILES, include/pureunix/task.h */
    case _SC_PAGESIZE: return 4096;
    default:
        errno = EINVAL;
        return -1;
    }
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

/* -------------------------------------------------------------------- */
/* Shell-style glob matching — newlib's <fnmatch.h> (third_party/newlib)   */
/* declares fnmatch() but ships no implementation for this target (same   */
/* "header present, .o absent" gap as getpwnam()/getrlimit() above); real  */
/* POSIX regcomp()/regexec() (needed by grep's actual regex matching) is a */
/* separate, much larger follow-up — this is deliberately just the glob   */
/* subset ash's bash-compat test operators and find -name/-path need.     */
/* -------------------------------------------------------------------- */

static int fnmatch_bracket(const char **pp, char c, int flags)
{
    const char *p = *pp + 1; /* just past '[' */
    bool negate = false;
    if (*p == '!' || *p == '^') {
        negate = true;
        p++;
    }
    bool matched = false;
    bool first = true;
    while (*p && (*p != ']' || first)) {
        first = false;
        char lo = *p++;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p) {
            lo = *p++;
        }
        char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = *p++;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p) {
                hi = *p++;
            }
        }
        if ((unsigned char)c >= (unsigned char)lo && (unsigned char)c <= (unsigned char)hi) {
            matched = true;
        }
    }
    if (*p == ']') {
        p++;
    }
    *pp = p;
    return negate ? !matched : matched;
}

static int fnmatch_match(const char *pat, const char *str, int flags)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') {
                pat++;
            }
            if (!*pat) {
                if (flags & FNM_PATHNAME) {
                    return strchr(str, '/') ? FNM_NOMATCH : 0;
                }
                return 0;
            }
            for (const char *s = str; ; s++) {
                if (fnmatch_match(pat, s, flags) == 0) {
                    return 0;
                }
                if (!*s || ((flags & FNM_PATHNAME) && *s == '/')) {
                    return FNM_NOMATCH;
                }
            }
        }
        if (!*str) {
            return FNM_NOMATCH;
        }
        if ((flags & FNM_PATHNAME) && *str == '/' && *pat != '/') {
            return FNM_NOMATCH;
        }
        if (*pat == '?') {
            if ((flags & FNM_PATHNAME) && *str == '/') {
                return FNM_NOMATCH;
            }
            pat++;
            str++;
            continue;
        }
        if (*pat == '[') {
            if ((flags & FNM_PATHNAME) && *str == '/') {
                return FNM_NOMATCH;
            }
            const char *save = pat;
            if (!fnmatch_bracket(&pat, *str, flags)) {
                return FNM_NOMATCH;
            }
            (void)save;
            str++;
            continue;
        }
        char pc = *pat;
        if (pc == '\\' && !(flags & FNM_NOESCAPE) && pat[1]) {
            pc = pat[1];
            pat++;
        }
        if (pc != *str) {
            return FNM_NOMATCH;
        }
        pat++;
        str++;
    }
    return *str ? FNM_NOMATCH : 0;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    if ((flags & FNM_PERIOD) && *string == '.' && *pattern != '.') {
        return FNM_NOMATCH;
    }
    return fnmatch_match(pattern, string, flags);
}

/* -------------------------------------------------------------------- */
/* Resource limits — no enforcement model exists in this kernel at all    */
/* (see user/newlib_compat/sys/resource.h), so these just report/accept   */
/* "unlimited" without recording anything, purely so ash's `ulimit`        */
/* builtin (shell/shell_common.c) links and runs.                         */
/* -------------------------------------------------------------------- */

/* Only the one case BusyBox's dd applet actually needs (an anonymous,
 * private scratch buffer — see user/newlib_compat/sys/mman.h) — backed by
 * malloc()/free() rather than a real page mapping, since this kernel has no
 * mmap/munmap/brk syscall at all (docs/syscalls.md). */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    (void)addr;
    (void)prot;
    (void)offset;
    if (fd != -1 || !(flags & MAP_ANONYMOUS)) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    void *p = malloc(length);
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

int munmap(void *addr, size_t length)
{
    (void)length;
    free(addr);
    return 0;
}

/* PureUNIX's VMM never sets a page non-writable or non-executable for user
 * code (kernel/elf.c maps every user page PAGE_USER|PAGE_WRITE, with no
 * NX-bit/execute-disable support at all — see docs/memory.md) — so every
 * byte a process can address is already simultaneously readable, writable,
 * and executable, unconditionally. A real mprotect() would need per-page
 * protection bits this kernel doesn't track; returning success without
 * doing anything is not a shortcut here, it's an accurate description of
 * this kernel's actual (weak) memory-protection model. Needed by TinyCC's
 * tccrun.c (see third_party/tcc/README.md) for `-run`'s set_pages_executable().
 */
int mprotect(void *addr, size_t length, int prot)
{
    (void)addr;
    (void)length;
    (void)prot;
    return 0;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    (void)resource;
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    rlim->rlim_cur = RLIM_INFINITY;
    rlim->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    (void)resource;
    (void)rlim;
    return 0;
}

/* -------------------------------------------------------------------- */
/* User/group database — real, backed by /etc/passwd (the same flat       */
/* "name:x:uid:gid:gecos:home:shell" file kernel/users.c's login flow      */
/* reads), not a stub. PureUNIX has no separate /etc/group file at all —   */
/* every account gets an implicit one-member primary group named after     */
/* itself (kernel/users.c's adduser always sets gid == uid), so             */
/* getgrnam()/getgrgid() resolve group identity by scanning /etc/passwd     */
/* for the account whose gid matches, exactly mirroring that real on-disk  */
/* data model rather than inventing a separate fake group database.        */
/* -------------------------------------------------------------------- */

static struct passwd pwgr_pw;
static char pwgr_pw_name[64], pwgr_pw_dir[128], pwgr_pw_shell[64];
static struct group pwgr_gr;
static char pwgr_gr_name[64];
static char *pwgr_gr_mem[1] = { NULL };

/* Reads one field at a time out of /etc/passwd via real file I/O (open/
 * read/close), matching a single predicate over (name, uid, gid). Returns
 * 0 and fills every *_out pointer (any of which may be NULL to skip it) on
 * a match, -1 (errno untouched) if no line matches. */
static int passwd_scan(const char *want_name, int want_uid_valid, uid_t want_uid,
                        int want_gid_valid, gid_t want_gid,
                        char *name_out, size_t name_cap,
                        uid_t *uid_out, gid_t *gid_out,
                        char *home_out, size_t home_cap,
                        char *shell_out, size_t shell_cap)
{
    int fd = open("/etc/passwd", 0);
    if (fd < 0) {
        return -1;
    }
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    char *save_line;
    char *line = strtok_r(buf, "\n", &save_line);
    while (line) {
        char linecopy[256];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *save_field;
        char *name  = strtok_r(linecopy, ":", &save_field);
        strtok_r(NULL, ":", &save_field); /* password placeholder */
        char *uid_s = strtok_r(NULL, ":", &save_field);
        char *gid_s = strtok_r(NULL, ":", &save_field);
        strtok_r(NULL, ":", &save_field); /* gecos */
        char *home_s  = strtok_r(NULL, ":", &save_field);
        char *shell_s = strtok_r(NULL, ":", &save_field);

        uid_t uid = uid_s ? (uid_t)atoi(uid_s) : 0;
        gid_t gid = gid_s ? (gid_t)atoi(gid_s) : 0;

        bool match = name != NULL;
        if (match && want_name && strcmp(name, want_name) != 0) match = false;
        if (match && want_uid_valid && uid != want_uid) match = false;
        if (match && want_gid_valid && gid != want_gid) match = false;

        if (match) {
            if (name_out) { strncpy(name_out, name, name_cap - 1); name_out[name_cap - 1] = '\0'; }
            if (uid_out) *uid_out = uid;
            if (gid_out) *gid_out = gid;
            if (home_out) { strncpy(home_out, home_s ? home_s : "/", home_cap - 1); home_out[home_cap - 1] = '\0'; }
            if (shell_out) { strncpy(shell_out, shell_s ? shell_s : "/bin/sh", shell_cap - 1); shell_out[shell_cap - 1] = '\0'; }
            return 0;
        }
        line = strtok_r(NULL, "\n", &save_line);
    }
    return -1;
}

struct passwd *getpwnam(const char *name)
{
    uid_t uid, gid;
    if (passwd_scan(name, 0, 0, 0, 0,
                     pwgr_pw_name, sizeof(pwgr_pw_name), &uid, &gid,
                     pwgr_pw_dir, sizeof(pwgr_pw_dir),
                     pwgr_pw_shell, sizeof(pwgr_pw_shell)) != 0) {
        return NULL;
    }
    pwgr_pw.pw_name = pwgr_pw_name;
    pwgr_pw.pw_passwd = (char *)"x";
    pwgr_pw.pw_uid = uid;
    pwgr_pw.pw_gid = gid;
    pwgr_pw.pw_comment = (char *)"";
    pwgr_pw.pw_gecos = (char *)"";
    pwgr_pw.pw_dir = pwgr_pw_dir;
    pwgr_pw.pw_shell = pwgr_pw_shell;
    return &pwgr_pw;
}

struct passwd *getpwuid(uid_t uid)
{
    gid_t gid;
    if (passwd_scan(NULL, 1, uid, 0, 0,
                     pwgr_pw_name, sizeof(pwgr_pw_name), NULL, &gid,
                     pwgr_pw_dir, sizeof(pwgr_pw_dir),
                     pwgr_pw_shell, sizeof(pwgr_pw_shell)) != 0) {
        return NULL;
    }
    pwgr_pw.pw_name = pwgr_pw_name;
    pwgr_pw.pw_passwd = (char *)"x";
    pwgr_pw.pw_uid = uid;
    pwgr_pw.pw_gid = gid;
    pwgr_pw.pw_comment = (char *)"";
    pwgr_pw.pw_gecos = (char *)"";
    pwgr_pw.pw_dir = pwgr_pw_dir;
    pwgr_pw.pw_shell = pwgr_pw_shell;
    return &pwgr_pw;
}

struct group *getgrnam(const char *name)
{
    uid_t uid;
    gid_t gid;
    if (passwd_scan(name, 0, 0, 0, 0, pwgr_gr_name, sizeof(pwgr_gr_name),
                     &uid, &gid, NULL, 0, NULL, 0) != 0) {
        return NULL;
    }
    pwgr_gr.gr_name = pwgr_gr_name;
    pwgr_gr.gr_passwd = (char *)"x";
    pwgr_gr.gr_gid = gid;
    pwgr_gr.gr_mem = pwgr_gr_mem;
    return &pwgr_gr;
}

struct group *getgrgid(gid_t gid)
{
    uid_t uid;
    gid_t found_gid;
    if (passwd_scan(NULL, 0, 0, 1, gid, pwgr_gr_name, sizeof(pwgr_gr_name),
                     &uid, &found_gid, NULL, 0, NULL, 0) != 0) {
        return NULL;
    }
    pwgr_gr.gr_name = pwgr_gr_name;
    pwgr_gr.gr_passwd = (char *)"x";
    pwgr_gr.gr_gid = gid;
    pwgr_gr.gr_mem = pwgr_gr_mem;
    return &pwgr_gr;
}
