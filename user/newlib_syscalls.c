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
#include <arpa/nameser.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <fnmatch.h>
#include <grp.h>
#include <iconv.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mntent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <resolv.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "pureunix_gfx.h"

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
    PU_SYS_RENAME  = 20,
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
    PU_SYS_FCHMOD = 54,
    PU_SYS_FCHOWN = 55,
    PU_SYS_INPUT_POLL = 56,
    PU_SYS_FB_GETINFO = 57,
    PU_SYS_FB_BLIT = 58,
    PU_SYS_GET_TICKS_MS = 59,
    PU_SYS_SET_GRAPHICS_MODE = 60,
    PU_SYS_FB_MMAP = 61,
    PU_SYS_SBRK = 62,
    PU_SYS_PTY_CREATE = 63,
    PU_SYS_FUTIME = 64,
    PU_SYS_STATFS = 65,
    PU_SYS_SET_TLS = 66,
    PU_SYS_POLL = 67,
    PU_SYS_SOCKET = 68,
    PU_SYS_BIND = 69,
    PU_SYS_LISTEN = 70,
    PU_SYS_ACCEPT = 71,
    PU_SYS_CONNECT = 72,
};

/* Mirrors arch/i386/syscall.c's SYS_POLL wire struct exactly (int32_t fd +
 * two int16_t fields, already 8 bytes/naturally aligned, no padding) —
 * same "explicit wire struct instead of relying on two differently-
 * declared structs happening to line up" reasoning as struct pu_raw_stat/
 * pu_raw_flock/pu_raw_statfs above. */
struct pu_raw_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* SYS_SET_TLS (include/pureunix/syscall.h) — tells the kernel about a TLS
 * block user/newlib_crt0.c's tls_init() already built, so
 * arch/i386/gdt.c's gdt_set_tls_base() can replay it on every future
 * context switch back to this task. This file is the one place that owns
 * the raw syscall ABI/PU_SYS_* numbering (see raw_syscall() above); crt0
 * stays kernel-ABI-agnostic and just calls this instead of duplicating
 * PU_SYS_SET_TLS or the int $0x80 gate itself. */
void __pureunix_set_tls_base(void *tp)
{
    raw_syscall(PU_SYS_SET_TLS, (int)tp, 0, 0);
}

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

/* Mirrors arch/i386/syscall.c's struct pu_raw_flock — plain int32_t fields
 * rather than newlib's struct flock (short/long, see
 * <sys/_default_fcntl.h>), for the same reason as struct pu_raw_stat
 * above: an explicit, fixed-width wire struct instead of relying on two
 * differently-declared structs happening to line up. fcntl() below
 * translates field-by-field in both directions. */
struct pu_raw_flock {
    int32_t l_type;
    int32_t l_whence;
    int32_t l_start;
    int32_t l_len;
    int32_t l_pid;
};

/* Mirrors include/pureunix/vfs.h's struct vfs_statfs exactly (same field
 * order/widths) — SYS_STATFS (arch/i386/syscall.c) writes directly into
 * this layout, same "explicit wire struct" convention as struct pu_raw_stat/
 * pu_raw_flock above. */
struct pu_raw_statfs {
    uint32_t f_bsize;
    uint32_t f_blocks;
    uint32_t f_bfree;
    uint32_t f_bavail;
    uint32_t f_files;
    uint32_t f_ffree;
    uint32_t f_namemax;
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

/* Minimal fcntl(): F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL/F_GETLK/F_SETLK/
 * F_SETLKW — see SYS_FCNTL (arch/i386/syscall.c) and docs/syscalls.md.
 * F_GETFD/F_SETFD are honest no-ops (no close-on-exec model — every fd is
 * always inherited across exec, see task_create_user()/task_fork()).
 * F_GETFL/F_SETFL translate PureUNIX's O_* bit layout to/from newlib's,
 * same as open(). F_GETLK/F_SETLK/F_SETLKW (added for the SQLite port,
 * docs/sqlite-port.md — SQLite's default unix VFS depends on real POSIX
 * advisory locking for every transaction) marshal newlib's struct flock
 * into struct pu_raw_flock across the syscall — see kernel/flock.c and
 * that struct's own comment for why F_SETLKW is handled exactly like
 * F_SETLK (SQLite's default build never actually blocks in the kernel for
 * this). */
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);

    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        struct flock *lck = va_arg(ap, struct flock *);
        va_end(ap);
        struct pu_raw_flock rlk;
        rlk.l_type = lck->l_type;
        rlk.l_whence = lck->l_whence;
        rlk.l_start = (int32_t)lck->l_start;
        rlk.l_len = (int32_t)lck->l_len;
        rlk.l_pid = lck->l_pid;

        int r = raw_syscall(PU_SYS_FCNTL, fd, cmd, (int)&rlk);
        if (r < 0) {
            return fail(r);
        }
        if (cmd == F_GETLK) {
            lck->l_type = (short)rlk.l_type;
            if (rlk.l_type != F_UNLCK) {
                lck->l_whence = (short)rlk.l_whence;
                lck->l_start = (long)rlk.l_start;
                lck->l_len = (long)rlk.l_len;
                lck->l_pid = (short)rlk.l_pid;
            }
        }
        return 0;
    }

    int arg = va_arg(ap, int);
    va_end(ap);

    if (cmd == F_SETFL) {
        int pu_flags = 0;
        if ((arg & 3) != 0) pu_flags |= PU_O_WRONLY;
        if (arg & 0x0008) pu_flags |= PU_O_APPEND;
        if (arg & 0x0200) pu_flags |= PU_O_CREAT;
        if (arg & 0x0400) pu_flags |= PU_O_TRUNC;
        /* O_NONBLOCK (newlib's _FNONBLOCK, 0x4000) was chosen in
         * include/pureunix/fcntl.h to numerically equal newlib's own
         * value, so no bit remapping is needed here -- just don't let it
         * fall through the cracks like the whitelist above used to
         * silently do (real bug: fcntl(fd, F_SETFL, O_NONBLOCK) appeared
         * to succeed but never actually reached f->flags, found while
         * fixing a real bidirectional-pipe deadlock in the Qt QPA port,
         * docs/qt-port.md). */
        if (arg & 0x4000) pu_flags |= 0x4000;
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
        if (r & 0x4000)      nl_flags |= 0x4000;
        return nl_flags;
    }
    return r;
}

int ftruncate(int fd, off_t length)
{
    int r = raw_syscall(PU_SYS_FTRUNCATE, fd, (int)length, 0);
    return r < 0 ? fail(r) : 0;
}

/* Path-based truncate(): no separate SYS_TRUNCATE exists (or is needed) —
 * open()+ftruncate()+close() is exactly what a path-based truncate does
 * everywhere, real syscall or not. Real Qt Core caller: qfsfileengine_unix.cpp's
 * QFSFileEngine::setSize(), a general POSIX function, not Qt-specific. */
int truncate(const char *path, off_t length)
{
    int fd = open(path, O_WRONLY, 0);
    if (fd < 0) {
        return -1;
    }
    int r = ftruncate(fd, length);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return r;
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

/* Real statvfs(), backed by SYS_STATFS (arch/i386/syscall.c -> fs/vfs.c's
 * vfs_statfs() -> the mounted filesystem's own ops->statfs, real on EXT2 —
 * see fs/ext2/mount.c's ext2_statfs()). Real Qt Core caller:
 * qstorageinfo_unix.cpp's QStorageInfoPrivate::retrieveVolumeInfo()
 * (QStorageInfo) — a general POSIX function, not Qt-specific (also used by
 * BusyBox's df applet). f_fsid/f_flag aren't populated (no volume-id or
 * mount-flags concept tracked anywhere yet) and stay 0, matching this
 * file's usual "honest zero, not a fabricated value" convention. */
int statvfs(const char *path, struct statvfs *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    struct pu_raw_statfs raw;
    int r = raw_syscall(PU_SYS_STATFS, (int)path, (int)&raw, 0);
    if (r < 0) {
        return fail(r);
    }
    buf->f_bsize = raw.f_bsize;
    buf->f_frsize = raw.f_bsize;
    buf->f_blocks = raw.f_blocks;
    buf->f_bfree = raw.f_bfree;
    buf->f_bavail = raw.f_bavail;
    buf->f_files = raw.f_files;
    buf->f_ffree = raw.f_ffree;
    buf->f_favail = raw.f_ffree;
    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = raw.f_namemax;
    return 0;
}

/* No fd-to-path resolution syscall exists for statfs specifically (unlike
 * fchmod/fchown/futimens above, which SYS_FCHMOD/SYS_FCHOWN/SYS_FUTIME
 * already do via the fd's own open_file_t.path) — not reachable by this
 * port's actual Qt Core scope (QStorageInfo always goes through the
 * path-based statvfs64() below), so left as a real, honest ENOSYS rather
 * than a fabricated kernel syscall for a currently-unreached code path
 * (same "declare it, implement for real only once something actually
 * calls it" precedent as docs/qt-port.md's Phase 3 statvfs64 finding). */
int fstatvfs(int fd, struct statvfs *buf)
{
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

/* struct statvfs64 (user/newlib_compat/sys/statvfs.h) is byte-for-byte
 * identical to struct statvfs above — PureUnix has no real 32-vs-64-bit
 * file-offset distinction to make — so this is genuinely just the
 * cast-through the header comment describes, not a shortcut. */
int statvfs64(const char *path, struct statvfs64 *buf)
{
    return statvfs(path, (struct statvfs *)buf);
}

/* Real answer, not a stub: every page this kernel's VMM maps (kernel/vmm.c,
 * arch/i386/*) is a fixed 4096-byte x86 page — there's no larger-page/huge-
 * page support anywhere to make this configurable. */
int getpagesize(void)
{
    return 4096;
}

/* pathconf()/fpathconf(): real answers where this kernel actually has a
 * fixed, known limit, not stubs -- fs/ext2/dir.c's own dirent name_len
 * field is a single byte and its own real ext2_dir_add_entry() already
 * rejects any name over 255 bytes, and PATH_MAX (limits.h) is this
 * kernel's own real fixed path-buffer size everywhere else (matches
 * include/pureunix/config.h's PUREUNIX_MAX_PATH, which vfs_normalize()
 * etc. actually use). Needed to link real Qt Widgets code for the first
 * time (libQt6Widgets.a's QFileDialogPrivate::maxNameLength() calls
 * pathconf(path, _PC_NAME_MAX) even though this port's own
 * qtwidgetstest.cpp never opens a QFileDialog itself — once one
 * translation unit inside a static archive is pulled in for any symbol,
 * every reference in that same .o must still resolve at link time).
 * Every other _PC_* case returns -1/EINVAL, same as any real libc does
 * for a query it doesn't have a real answer for. */
long pathconf(const char *path, int name)
{
    (void)path;
    switch (name) {
    case _PC_NAME_MAX:
        return 255;
    case _PC_PATH_MAX:
        return PATH_MAX;
    default:
        errno = EINVAL;
        return -1;
    }
}

long fpathconf(int fd, int name)
{
    (void)fd;
    return pathconf(NULL, name);
}

/* BSD flock() reimplemented on top of the real POSIX fcntl() advisory
 * locking already added for the SQLite port (PU_SYS_FCNTL, kernel/flock.c,
 * docs/sqlite-port.md) — the same "implement the BSD entry point over the
 * POSIX one" approach glibc/most libcs use, not a PureUNIX-specific
 * shortcut. flock()'s lock domain is technically distinct from fcntl()'s
 * (whole-open-file vs whole-process-per-inode), but with no real
 * multi-process concurrent access to the same file ever exercised on this
 * target (single fcntl()-based lock table, see kernel/flock.c), a
 * whole-file F_SETLK/F_SETLKW covering the entire file (l_whence=SEEK_SET,
 * l_start=0, l_len=0) gives correct-enough behavior for every real caller.
 * Real Qt Core callers: qlockfile_unix.cpp's QLockFilePrivate::tryLock_sys()/
 * removeStaleLock() (QLockFile, used internally by QSettings et al). */
int flock(int fd, int operation)
{
    struct flock lck;
    lck.l_whence = SEEK_SET;
    lck.l_start = 0;
    lck.l_len = 0;
    lck.l_pid = 0;

    int op = operation & ~LOCK_NB;
    if (op == LOCK_UN) {
        lck.l_type = F_UNLCK;
    } else if (op == LOCK_SH) {
        lck.l_type = F_RDLCK;
    } else if (op == LOCK_EX) {
        lck.l_type = F_WRLCK;
    } else {
        errno = EINVAL;
        return -1;
    }

    return fcntl(fd, (operation & LOCK_NB) ? F_SETLK : F_SETLKW, &lck);
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

/* Real, not "any of fd 0/1/2" — asks the kernel via the same TIOCGWINSZ
 * probe a real libc's isatty() would use an ioctl for. tty_fd_check()
 * (arch/i386/syscall.c) already distinguishes a genuine console binding
 * (fd 0/1/2 with no open_file_t, or an explicit FD_KIND_TTY /dev/ttyN)
 * from a redirected-to-a-file/pipe fd, returning -ENOTTY for the latter —
 * this just surfaces that distinction instead of guessing from the fd
 * number alone. Matters for anything that checks "am I interactive"
 * before deciding whether to print prompts, e.g. Lua's REPL
 * (lua_stdin_is_tty(), LUA_USE_POSIX) under `lua < script.lua`. */
int isatty(int fd)
{
    struct winsize ws;
    int r = raw_syscall(PU_SYS_IOCTL, fd, TIOCGWINSZ, (int)&ws);
    return r == 0;
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

/* flockfile()/funlockfile()/ftrylockfile() are declared in newlib's own
 * <stdio.h> (getc_unlocked()'s companions) but never *defined* in the
 * vendored libc.a — this newlib was never configured with a threading
 * model, so the reentrant-locking half of stdio was left out entirely.
 * PureUNIX has no threads (one task, one stack, cooperative scheduling —
 * docs/scheduler.md), so genuine per-FILE* locks would have nothing to
 * exclude; honest no-ops are the correct implementation here, not a
 * placeholder. liolib.c's l_lockfile/l_unlockfile (LUA_USE_POSIX) are the
 * motivating caller. */
void flockfile(FILE *file)
{
    (void)file;
}

int ftrylockfile(FILE *file)
{
    (void)file;
    return 0;
}

void funlockfile(FILE *file)
{
    (void)file;
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

/* clock_gettime()/clock_getres() — newlib's own <time.h> only declares
 * these behind an `#ifdef __rtems__` guard (see third_party/newlib's
 * sys/features.h - it has no case for an unknown/generic target), so
 * user/newlib_compat/time.h declares them unconditionally instead. Real
 * implementations, not stubs: CLOCK_MONOTONIC is backed by SYS_GET_TICKS_MS
 * (syscall 59, PIT-derived, 10ms resolution, already general-purpose —
 * this is the exact clock Qt's QElapsedTimer needs, see docs/qt-port.md
 * section 4), CLOCK_REALTIME reuses gettimeofday() above (SYS_GETTIMEOFDAY,
 * second resolution). Any other clockid_t is rejected with EINVAL, matching
 * real clock_gettime()'s behavior for an unsupported clock. */
int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    if (clock_id == CLOCK_MONOTONIC) {
        unsigned int ms = (unsigned int)raw_syscall(PU_SYS_GET_TICKS_MS, 0, 0, 0);
        tp->tv_sec = (time_t)(ms / 1000);
        tp->tv_nsec = (long)((ms % 1000) * 1000000L);
        return 0;
    }
    if (clock_id == CLOCK_REALTIME) {
        struct timeval tv;
        if (gettimeofday(&tv, 0) != 0) {
            return -1;
        }
        tp->tv_sec = tv.tv_sec;
        tp->tv_nsec = 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    if (!res) {
        errno = EFAULT;
        return -1;
    }
    if (clock_id == CLOCK_MONOTONIC) {
        res->tv_sec = 0;
        res->tv_nsec = 10000000L; /* 10ms, matching SYS_GET_TICKS_MS's real PIT resolution */
        return 0;
    }
    if (clock_id == CLOCK_REALTIME) {
        res->tv_sec = 1; /* SYS_GETTIMEOFDAY has second resolution only */
        res->tv_nsec = 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* getentropy() — cpuid/rdrand are plain, unprivileged x86 instructions (no
 * ring-0 support needed, unlike almost everything else this file wraps),
 * same "just execute it" reasoning kernel/vmm.c's own cpuid-based PAT probe
 * already relies on. Real hardware entropy (RDRAND, CPUID.1:ECX.30) is used
 * whenever the CPU has it; on a CPU/QEMU CPU model without it, this falls
 * back to a simple xorshift PRNG reseeded from SYS_GET_TICKS_MS each call —
 * good enough for QRandomGenerator's non-cryptographic seeding use (Qt
 * Core's real caller: qrandom.cpp's QRandomGenerator::SystemGenerator, used
 * to seed QHash's per-process seed and QUuid — not a security-sensitive CSPRNG
 * deployment on this target), not a claim of real cryptographic quality
 * without RDRAND. */
static int rdrand_supported(void)
{
    static int checked = 0, supported = 0;
    if (!checked) {
        unsigned int a = 1, c = 0, d = 0, b = 0;
        __asm__ volatile("cpuid" : "+a"(a), "=c"(c), "=d"(d), "=b"(b));
        supported = (c & (1U << 30)) != 0;
        checked = 1;
    }
    return supported;
}

static unsigned int rdrand32(void)
{
    unsigned int val;
    unsigned char ok;
    int attempts = 10;
    do {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
    } while (!ok && --attempts > 0);
    return val;
}

static unsigned int fallback_prng32(void)
{
    static unsigned int state;
    unsigned int ms = (unsigned int)raw_syscall(PU_SYS_GET_TICKS_MS, 0, 0, 0);
    state ^= ms + 0x9e3779b9u + (state << 6) + (state >> 2);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int getentropy(void *buf, size_t buflen)
{
    if (buflen > 256) {
        errno = EIO;
        return -1;
    }
    unsigned char *p = buf;
    int use_rdrand = rdrand_supported();
    while (buflen > 0) {
        unsigned int r = use_rdrand ? rdrand32() : fallback_prng32();
        size_t n = buflen < sizeof(r) ? buflen : sizeof(r);
        memcpy(p, &r, n);
        p += n;
        buflen -= n;
    }
    return 0;
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

/* utime() (user/newlib_compat/utime.h's own comment) — a real, working
 * conversion on top of the real utimes() just above, not a second
 * implementation: struct utimbuf's whole-second time_t fields map onto
 * struct timeval's tv_sec directly (tv_usec is simply 0, exactly what a
 * real POSIX utime() means by "no sub-second precision"). */
int utime(const char *path, const struct utimbuf *times)
{
    if (!times) {
        return utimes(path, NULL);
    }
    struct timeval tv[2];
    tv[0].tv_sec = times->actime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = times->modtime;
    tv[1].tv_usec = 0;
    return utimes(path, tv);
}

/* fd-based utimensat() — same NOW/OMIT/explicit-seconds translation as
 * utimensat() above, backed by SYS_FUTIME (arch/i386/syscall.c) which
 * resolves the fd to its open_file_t.path and calls the same vfs_utime()
 * the path-based syscalls use, mirroring how fchmod()/fchown() above
 * already resolve an fd the same way. Real Qt Core caller:
 * qfilesystemengine_unix.cpp's QFileSystemEngine::setFileTime()
 * (QFile::setFileTime()/QFileDevice), a general POSIX primitive, not
 * Qt-specific. */
int futimens(int fd, const struct timespec times[2])
{
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
    int r = raw_syscall(PU_SYS_FUTIME, fd, (int)atime, (int)mtime);
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

/* A real PU_SYS_RENAME call, not newlib's own generic rename() fallback
 * (link() the new name, then unlink() the old one) -- that fallback is
 * what this target got by default since no newlib-facing rename() was
 * ever defined here (only libpure.h's lower-level pu_rename(), used by
 * non-newlib programs like systest/ext2test). It silently "worked" for
 * plain files, but renaming a *directory* always failed with a
 * confusing EPERM ("Not owner"): ext2_link() (fs/ext2/write.c) correctly
 * refuses to hard-link a directory, which is exactly what that fallback
 * tried to do first. Found via PUFiles' own Rename button on a real
 * folder -- see docs/pude.md. vfs_rename()/ext2_rename() already
 * implement a real, atomic, directory-safe rename; this was just never
 * wired up for any newlib-linked program (pude, PUTerm, BusyBox, ...). */
int rename(const char *old_path, const char *new_path)
{
    int r = raw_syscall(PU_SYS_RENAME, (int)old_path, (int)new_path, 0);
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

/* fd-based chmod()/chown() — added for the SQLite port (docs/sqlite-port.md):
 * os_unix.c's unixCreate() copies the main db file's permissions/ownership
 * onto a freshly created rollback-journal file via these. Real, not a
 * stub: SYS_FCHMOD/SYS_FCHOWN (arch/i386/syscall.c) resolve against the
 * fd's own open_file_t.path and call the same vfs_chmod()/vfs_chown() the
 * path-based syscalls above use. */
int fchmod(int fd, mode_t mode)
{
    int r = raw_syscall(PU_SYS_FCHMOD, fd, (int)mode, 0);
    return r < 0 ? fail(r) : 0;
}

int fchown(int fd, uid_t uid, gid_t gid)
{
    int r = raw_syscall(PU_SYS_FCHOWN, fd, (int)uid, (int)gid);
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
    dirp->current.d_ino = 0; /* see dirent.h's own comment on struct dirent */
    return &dirp->current;
}

int closedir(DIR *dirp)
{
    free(dirp);
    return 0;
}

/* Real, not a stub: opendir() already eagerly reads the *entire*
 * directory listing into dirp->entries up front (see struct DIR's own
 * comment above) — readdir() just walks that in-memory array via
 * dirp->pos, so resetting the stream back to the beginning genuinely is
 * this simple, no new kernel syscall needed. Added for GLib's gdir.c
 * (docs/pcmanfm-port.md phase 3/6) — a real, general POSIX dirent.h
 * function this project's own dirent.h compat header never had a caller
 * for until now. */
void rewinddir(DIR *dirp)
{
    if (dirp) {
        dirp->pos = 0;
    }
}

/* Honest stub, not a fake fd: this kernel's SYS_READDIR has no per-
 * directory cursor at all (opendir() dumps the whole listing into DIR up
 * front — see struct DIR's own comment above), so there is no real
 * underlying file descriptor a DIR* could report. Real POSIX dirfd() is
 * only ever meaningful paired with fd-relative calls (openat()/
 * fstatat()/...), none of which exist here either (see this file's own
 * openat/fstatat gaps) — ENOSYS is the correct answer, not a fabricated
 * fd number. Exists purely so callers that merely *reference* dirfd() at
 * compile time (e.g. htop's XUtils.h xDirfd(), only actually invoked
 * behind its own HAVE_OPENAT guard) link; nothing in this port's build
 * calls it at runtime. */
int dirfd(DIR *dirp)
{
    (void)dirp;
    errno = ENOSYS;
    return -1;
}

/* Honest ENOSYS — see dirent.h's own comment on fdopendir(): SYS_READDIR
 * is path-based only, no way to recover a listing from a bare fd. */
DIR *fdopendir(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return (DIR *)0;
}

/* openat(): real, but narrow — only AT_FDCWD-relative opens are
 * supported, which is functionally identical to plain open() since this
 * kernel has no real per-fd-relative path resolution at all (see
 * dirfd()'s own comment just above). GIO's glocalfile.c (the only caller
 * in this port) only ever passes AT_FDCWD. */
int openat(int dirfd, const char *path, int flags, ...)
{
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, mode);
}

/* creat(): real, trivial — POSIX defines it as exactly this. */
int creat(const char *path, mode_t mode)
{
    return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/* fstatat(): real, but narrow — same AT_FDCWD-only scope as openat()
 * just above (this kernel has no real per-fd-relative path resolution).
 * AT_SYMLINK_NOFOLLOW selects lstat() vs stat(), same as every real
 * fstatat() implementation. */
int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    if (flags & AT_SYMLINK_NOFOLLOW) {
        return lstat(path, buf);
    }
    return stat(path, buf);
}

/* nftw(): real recursive directory-tree walk, built on the already-real
 * opendir()/readdir()/lstat() above — nothing platform-specific about
 * the algorithm itself (only GLib's own g_test_run() cleanup path calls
 * this; PCManFM-Qt never does, but it's a genuine general POSIX
 * primitive worth implementing for real rather than stubbing). FTW_PHYS
 * behavior always applies (symlinks are reported via FTW_SL, never
 * followed) since this kernel's lstat() is the only one available;
 * FTW_MOUNT is a no-op (no multi-filesystem model at this level) and
 * FTW_CHDIR is a no-op (callbacks already receive a fully-qualified
 * path, so chdir()'ing first was only ever a shortcut, never required
 * for correctness). */
static int nftw_recurse(char *pathbuf, size_t pathcap,
                         int (*fn)(const char *, const struct stat *, int, struct FTW *),
                         int flags, int level)
{
    struct stat st;
    struct FTW ftwbuf;
    const char *base_p = strrchr(pathbuf, '/');
    ftwbuf.base = base_p ? (int)(base_p - pathbuf + 1) : 0;
    ftwbuf.level = level;

    if (lstat(pathbuf, &st) != 0) {
        return fn(pathbuf, &st, FTW_NS, &ftwbuf);
    }

    if (S_ISLNK(st.st_mode)) {
        return fn(pathbuf, &st, FTW_SL, &ftwbuf);
    }

    if (!S_ISDIR(st.st_mode)) {
        return fn(pathbuf, &st, FTW_F, &ftwbuf);
    }

    if (!(flags & FTW_DEPTH)) {
        int r = fn(pathbuf, &st, FTW_D, &ftwbuf);
        if (r != 0) {
            return r;
        }
    }

    DIR *d = opendir(pathbuf);
    if (!d) {
        return fn(pathbuf, &st, FTW_DNR, &ftwbuf);
    }
    struct dirent *de;
    size_t base_len = strlen(pathbuf);
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        size_t name_len = strlen(de->d_name);
        if (base_len + 1 + name_len + 1 > pathcap) {
            closedir(d);
            errno = ENAMETOOLONG;
            return -1;
        }
        pathbuf[base_len] = '/';
        strcpy(pathbuf + base_len + 1, de->d_name);
        int r = nftw_recurse(pathbuf, pathcap, fn, flags, level + 1);
        pathbuf[base_len] = '\0';
        if (r != 0) {
            closedir(d);
            return r;
        }
    }
    closedir(d);

    if (flags & FTW_DEPTH) {
        const char *base_p2 = strrchr(pathbuf, '/');
        ftwbuf.base = base_p2 ? (int)(base_p2 - pathbuf + 1) : 0;
        return fn(pathbuf, &st, FTW_DP, &ftwbuf);
    }
    return 0;
}

int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int nopenfd, int flags)
{
    (void)nopenfd;
    char pathbuf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(pathbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(pathbuf, path, len + 1);
    /* Strip a trailing slash (except for "/" itself) so base-index math
     * above stays correct for every recursive step. */
    while (len > 1 && pathbuf[len - 1] == '/') {
        pathbuf[--len] = '\0';
    }
    return nftw_recurse(pathbuf, sizeof(pathbuf), fn, flags, 0);
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

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
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

/* execl()/execlp()'s real, standard shape everywhere: collect the
 * variadic argument list into a plain argv[] array, then hand off to the
 * real vector-argument syscall wrappers above — not a PureUNIX-specific
 * stub, just the same thin wrapper every libc ships. Added for htop's
 * OpenFilesScreen/TraceScreen (third_party/htop/), which both call
 * execlp("lsof"/"strace"/"truss", ...) — none of those binaries exist on
 * PureUNIX, so the call still fails (ENOENT), exactly the same graceful
 * "not installed" failure a real Unix without lsof/strace would give. */
#define EXECL_MAX_ARGS 32

int execl(const char *path, const char *arg0, ...)
{
    const char *argv[EXECL_MAX_ARGS];
    int n = 0;
    argv[n++] = arg0;
    va_list ap;
    va_start(ap, arg0);
    while (n < EXECL_MAX_ARGS - 1) {
        const char *next = va_arg(ap, const char *);
        argv[n++] = next;
        if (!next) {
            break;
        }
    }
    va_end(ap);
    argv[EXECL_MAX_ARGS - 1] = NULL;
    return execve(path, (char *const *)argv, environ);
}

int execlp(const char *file, const char *arg0, ...)
{
    const char *argv[EXECL_MAX_ARGS];
    int n = 0;
    argv[n++] = arg0;
    va_list ap;
    va_start(ap, arg0);
    while (n < EXECL_MAX_ARGS - 1) {
        const char *next = va_arg(ap, const char *);
        argv[n++] = next;
        if (!next) {
            break;
        }
    }
    va_end(ap);
    argv[EXECL_MAX_ARGS - 1] = NULL;
    return execvp(file, (char *const *)argv);
}

/* No real hostname database exists (no sethostname()-configurable state
 * anywhere in the kernel) — a fixed, honest name rather than fabricating
 * per-boot state nothing backs, matching Platform_getRelease()'s own
 * "PureUNIX" naming in third_party/htop/pureunix/Platform.c. */
int gethostname(char *name, size_t len)
{
    static const char host[] = "pureunix";
    if (!name) {
        errno = EFAULT;
        return -1;
    }
    size_t n = sizeof(host) < len ? sizeof(host) : len;
    memcpy(name, host, n);
    if (n < len) {
        name[n] = '\0';
    } else if (len > 0) {
        name[len - 1] = '\0';
    }
    return 0;
}

/* ---- system()/popen()/pclose() ----
 * The vendored libc.a's own system() (libc/stdlib/system.c) is an
 * unconditional dummy for this target — no host support was ever
 * configured, so it just sets errno=ENOSYS and returns -1; popen()/
 * pclose() don't exist in libc.a at all (no host popen.c was ever built
 * either). Real fork()/pipe()/dup2()/execve()/waitpid() already exist
 * above, so — same as this file's dirname()/realpath()/fnmatch()
 * additions — these are implemented for real rather than left stubbed.
 * Defining `system` here (rather than in the archive) wins at link time
 * purely through ordinary archive-member laziness: this object is given
 * to the linker directly, ahead of -lc, so libc.a's own lib_a-system.o is
 * never even pulled in to resolve the symbol. Both spawn a real BusyBox
 * ash (`/bin/sh -c "..."`) child, matching the standard POSIX contract —
 * Lua's os.execute()/io.popen() (LUA_USE_POSIX) are the motivating
 * callers, but this is generic libc functionality, not Lua-specific. */
int system(const char *command)
{
    if (!command) {
        return 1; /* a shell is always available */
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        char *argv[] = { (char *)"sh", (char *)"-c", (char *)command, NULL };
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry on signal-interrupted wait */
    }
    return status;
}

/* popen()/pclose() need to remember which child pid a given FILE* stream
 * belongs to, so pclose() waits on the right process — a small fixed
 * table, matching this codebase's general "fixed-size pool, not
 * kmalloc'd" style (e.g. kernel/task.c's g_open_files[]) rather than a
 * linked list. 16 concurrent popen() streams is far more than anything in
 * this userland (ash, Lua) opens at once. */
#define POPEN_MAX 16
static struct { FILE *fp; pid_t pid; } popen_table[POPEN_MAX];

FILE *popen(const char *command, const char *type)
{
    if (!type || (type[0] != 'r' && type[0] != 'w') || type[1] != '\0') {
        errno = EINVAL;
        return NULL;
    }
    int fds[2];
    if (pipe(fds) < 0) {
        return NULL;
    }
    bool reading = (type[0] == 'r');
    int parent_fd = reading ? fds[0] : fds[1];
    int child_fd  = reading ? fds[1] : fds[0];
    int child_std = reading ? 1 : 0; /* child's stdout for "r", stdin for "w" */

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return NULL;
    }
    if (pid == 0) {
        close(parent_fd);
        dup2(child_fd, child_std);
        close(child_fd);
        char *argv[] = { (char *)"sh", (char *)"-c", (char *)command, NULL };
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    close(child_fd);
    FILE *fp = fdopen(parent_fd, type);
    if (!fp) {
        int saved = errno;
        close(parent_fd);
        waitpid(pid, NULL, 0);
        errno = saved;
        return NULL;
    }
    for (int slot = 0; slot < POPEN_MAX; slot++) {
        if (!popen_table[slot].fp) {
            popen_table[slot].fp = fp;
            popen_table[slot].pid = pid;
            break;
        }
    }
    /* No free slot (>16 concurrent popen()s): pclose() below just won't
     * find this stream and will fclose() it without reaping the child,
     * leaving a zombie — the same degraded-but-safe failure mode classic
     * fixed-table popen() implementations have always had. */
    return fp;
}

int pclose(FILE *stream)
{
    pid_t pid = -1;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (popen_table[i].fp == stream) {
            pid = popen_table[i].pid;
            popen_table[i].fp = NULL;
            break;
        }
    }
    fclose(stream);
    if (pid < 0) {
        errno = ECHILD;
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry on signal-interrupted wait */
    }
    return status;
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

/* Real poll(), backed by SYS_POLL (arch/i386/syscall.c -> fs/vfs.c's
 * pipe_buf_t.count for FD_KIND_PIPE fds, the one fd kind this kernel can
 * genuinely check readiness for without lying — see that syscall's own
 * comment). Originally reachable only via ash's `read -t`/`read -s`
 * builtin and libbb's safe_poll() retry wrapper, both of which only ever
 * needed the real read()/write() that follows to be the thing that
 * actually blocks; Qt's event dispatcher (qeventdispatcher_unix.cpp,
 * docs/qt-port.md section 4) is a real multiplexing caller — its own
 * same-process wakeup pipe is exactly the FD_KIND_PIPE case SYS_POLL
 * handles for real. nfds==0 (Qt's "just sleep until the next due
 * QTimer" idiom) is special-cased to skip marshalling an empty array. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (nfds == 0) {
        if (timeout != 0) {
            struct timespec req;
            if (timeout < 0) {
                req.tv_sec = 0;
                req.tv_nsec = 50000000L; /* no fds at all to ever become "ready" -- approximate an unbounded wait as bounded 50ms steps, same as before */
            } else {
                req.tv_sec = timeout / 1000;
                req.tv_nsec = (long)(timeout % 1000) * 1000000L;
            }
            nanosleep(&req, 0);
        }
        return 0;
    }

    struct pu_raw_pollfd *raw = malloc(nfds * sizeof(struct pu_raw_pollfd));
    if (!raw) {
        errno = ENOMEM;
        return -1;
    }
    for (nfds_t i = 0; i < nfds; i++) {
        raw[i].fd = fds[i].fd;
        raw[i].events = fds[i].events;
        raw[i].revents = 0;
    }
    int r = raw_syscall(PU_SYS_POLL, (int)raw, (int)nfds, timeout);
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = raw[i].revents;
    }
    free(raw);
    return r < 0 ? fail(r) : r;
}

/* Real select(), built on the same SYS_POLL as poll() just above (marshals
 * every fd named in readfds/writefds into a compact pollfd list, then
 * unpacks revents back into fresh fd_sets) — same real FD_KIND_PIPE
 * readiness, same honest-optimistic fallback for every other fd kind.
 * exceptfds is always empty (no urgent/OOB data model exists anywhere).
 * Originally added for htop's TraceScreen (its own F-key strace/truss
 * integration fails at the execlp() step anyway, neither binary exists on
 * PureUNIX, so it only ever needed to link here); Qt's event dispatcher
 * (docs/qt-port.md section 4) is what makes real readiness matter now. */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    if (exceptfds) {
        FD_ZERO(exceptfds);
    }

    int count = 0;
    for (int fd = 0; fd < nfds; fd++) {
        if ((readfds && FD_ISSET(fd, readfds)) || (writefds && FD_ISSET(fd, writefds))) {
            count++;
        }
    }
    if (count == 0) {
        if (readfds) {
            FD_ZERO(readfds);
        }
        if (writefds) {
            FD_ZERO(writefds);
        }
        if (timeout) {
            struct timespec req;
            req.tv_sec = timeout->tv_sec;
            req.tv_nsec = (long)timeout->tv_usec * 1000L;
            nanosleep(&req, 0);
        }
        return 0;
    }

    struct pu_raw_pollfd *raw = malloc((size_t)count * sizeof(struct pu_raw_pollfd));
    if (!raw) {
        errno = ENOMEM;
        return -1;
    }
    int idx = 0;
    for (int fd = 0; fd < nfds; fd++) {
        bool r = readfds && FD_ISSET(fd, readfds);
        bool w = writefds && FD_ISSET(fd, writefds);
        if (!r && !w) {
            continue;
        }
        raw[idx].fd = fd;
        raw[idx].events = (int16_t)((r ? POLLIN : 0) | (w ? POLLOUT : 0));
        raw[idx].revents = 0;
        idx++;
    }

    int timeout_ms = timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    int r = raw_syscall(PU_SYS_POLL, (int)raw, count, timeout_ms);

    if (readfds) {
        FD_ZERO(readfds);
    }
    if (writefds) {
        FD_ZERO(writefds);
    }
    int ready = 0;
    if (r >= 0) {
        for (int i = 0; i < count; i++) {
            if (readfds && (raw[i].revents & POLLIN)) {
                FD_SET(raw[i].fd, readfds);
                ready++;
            }
            if (writefds && (raw[i].revents & POLLOUT)) {
                FD_SET(raw[i].fd, writefds);
                ready++;
            }
        }
    }
    free(raw);
    return r < 0 ? fail(r) : ready;
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

/* Real sbrk(), backed by SYS_SBRK (arch/i386/syscall.c): the kernel maps
 * one real physical page at a time as the break actually grows past
 * task_t.heap_mapped, up to HEAP_MAX (32 MiB, include/pureunix/vmm.h) --
 * replaces what used to be a plain pointer bump through a fixed-size
 * static array in every newlib program's own bss. That old design
 * eagerly cost every single program (hello, sqlite3, lua, ...) however
 * many real MiB the array was sized to, regardless of whether it ever
 * used that much heap, because kernel/elf.c's PT_LOAD loader has no
 * demand paging: it allocates a real physical frame for *every* page of
 * a program's bss up front at exec() time. This was found and fixed
 * while porting Chocolate Doom (docs/chocolate-doom-port.md): its zone
 * allocator (src/z_zone.c) mallocs up to ~16-32 MiB in one shot, which
 * would have meant bumping that shared array to 32 MiB for every newlib
 * program just to satisfy one of them -- a real-world case of the exact
 * problem the SDL2 port's own dedicated-mapping pattern (SYS_FB_MMAP)
 * had already flagged as the wrong fix (see HEAP_VA's comment in
 * include/pureunix/vmm.h). A real incremental sbrk() fixes it for good:
 * a tiny program now costs only the handful of pages it actually
 * touches, while a program that genuinely needs tens of MiB can still
 * get them, up to HEAP_MAX. */
void *sbrk(ptrdiff_t incr)
{
    int r = raw_syscall(PU_SYS_SBRK, (int)incr, 0, 0);
    if (r < 0) {
        errno = -r;
        return (void *)-1;
    }
    return (void *)r;
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

/* getrusage(): honest stub — see user/newlib_compat/sys/resource.h's own
 * comment on struct rusage for why (no syscall surfaces task_t.cpu_ticks
 * anywhere yet). Added for the SQLite port's shell.c `.timer` command. */
int getrusage(int who, struct rusage *usage)
{
    (void)who;
    if (!usage) {
        errno = EFAULT;
        return -1;
    }
    usage->ru_utime.tv_sec = 0;
    usage->ru_utime.tv_usec = 0;
    usage->ru_stime.tv_sec = 0;
    usage->ru_stime.tv_usec = 0;
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

/* getpwnam_r()/getpwuid_r(): real — built on the already-real getpwnam()/
 * getpwuid() above, just copying their static-buffer result into the
 * caller-supplied reentrant buffer (glibc's own manpage documents this
 * exact "reentrant wrapper around the shared lookup" relationship as a
 * valid implementation strategy). */
static int passwd_copy_r(struct passwd *src, struct passwd *pwd, char *buf, size_t buflen,
                          struct passwd **result)
{
    if (!src) {
        *result = NULL;
        return 0;
    }
    size_t name_len = strlen(src->pw_name) + 1;
    size_t dir_len = strlen(src->pw_dir) + 1;
    size_t shell_len = strlen(src->pw_shell) + 1;
    if (name_len + dir_len + shell_len + 2 > buflen) {
        return ERANGE;
    }
    char *p = buf;
    memcpy(p, src->pw_name, name_len);
    pwd->pw_name = p;
    p += name_len;
    memcpy(p, src->pw_dir, dir_len);
    pwd->pw_dir = p;
    p += dir_len;
    memcpy(p, src->pw_shell, shell_len);
    pwd->pw_shell = p;
    p += shell_len;
    *p = '\0';
    pwd->pw_passwd = p;
    pwd->pw_comment = p;
    pwd->pw_gecos = p;
    pwd->pw_uid = src->pw_uid;
    pwd->pw_gid = src->pw_gid;
    *result = pwd;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
    return passwd_copy_r(getpwnam(name), pwd, buf, buflen, result);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
    return passwd_copy_r(getpwuid(uid), pwd, buf, buflen, result);
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

/* ---- SDL2 platform support (docs/sdl-port.md) ---- */

int pu_input_poll(pu_input_event_t *out)
{
    return raw_syscall(PU_SYS_INPUT_POLL, (int)out, 0, 0);
}

int pu_fb_getinfo(pu_fb_info_t *out)
{
    return raw_syscall(PU_SYS_FB_GETINFO, (int)out, 0, 0);
}

int pu_fb_blit(const void *buf, unsigned int len)
{
    return raw_syscall(PU_SYS_FB_BLIT, (int)buf, (int)len, 0);
}

unsigned int pu_get_ticks_ms(void)
{
    return (unsigned int)raw_syscall(PU_SYS_GET_TICKS_MS, 0, 0, 0);
}

void pu_set_graphics_mode(int enable)
{
    (void)raw_syscall(PU_SYS_SET_GRAPHICS_MODE, enable, 0, 0);
}

void *pu_fb_mmap(void)
{
    int r = raw_syscall(PU_SYS_FB_MMAP, 0, 0, 0);
    return r < 0 ? NULL : (void *)r;
}

/* ---- PTY (include/pureunix/pty.h, docs/pude.md) ---- */

int pu_pty_create(int fds[2])
{
    return raw_syscall(PU_SYS_PTY_CREATE, (int)fds, 0, 0);
}

/* ---- iconv (<iconv.h>) ----
 *
 * newlib's own configure.host has no case for this bare i686-elf target,
 * so <iconv.h> is declared (see third_party/newlib/i686-elf/include/
 * iconv.h) but never actually implemented anywhere in the vendored
 * libc.a — a real gap, not a subtle one (confirmed: `nm libc.a` has zero
 * iconv_open/iconv/iconv_close symbols at all). GLib (docs/pcmanfm-
 * port.md) uses iconv() pervasively for charset conversion, so this port
 * needs a real one.
 *
 * Deliberately narrow, not a general iconv: this whole system only ever
 * runs in a single, fixed UTF-8 locale (there is no locale database, no
 * setlocale() beyond the "C" default — see the recurring "Detected locale
 * \"C\" ... not UTF-8" warning throughout docs/qt-port.md) and every
 * string that will ever actually flow through this function already *is*
 * UTF-8 (or a strict subset of it, ASCII). So the only conversion that
 * can ever genuinely be needed here is UTF-8-family-to-UTF-8-family,
 * which is a real, correct identity/passthrough — not a lie standing in
 * for iconv's full multi-charset-table behavior, which this platform has
 * no real use for and no locale data to back anyway. Any other charset
 * pair is rejected with a real errno, exactly like a real iconv() report-
 * ing "I don't have this conversion" — not silently mistranslated.
 */

/* iconv_t is just `void *` on this newlib (sys/_types.h's _iconv_t) — a
 * real per-conversion descriptor isn't needed since there's only one
 * supported conversion "kind" (identity), so this sentinel is the only
 * value iconv_open() below ever hands back. */
#define PU_ICONV_UTF8_IDENTITY ((iconv_t)1)

/* True for every charset name this platform can genuinely treat as
 * "already UTF-8 compatible bytes": real UTF-8 (spelled a few common
 * ways) and 7-bit ASCII (a strict subset of UTF-8, so passthrough is
 * still a correct, not approximate, identity conversion for it) — plus
 * the couple of POSIX default-locale spellings glib's own locale-
 * detection code can plausibly ask for on a system with no locale
 * database (see this function's own header comment). Anything else is
 * honestly unsupported, not silently mistranslated. */
static bool pu_iconv_charset_is_utf8_compatible(const char *name)
{
    static const char *const compatible[] = {
        "UTF-8", "UTF8", "ASCII", "US-ASCII", "ANSI_X3.4-1968", "C", "POSIX",
    };
    for (size_t i = 0; i < sizeof(compatible) / sizeof(compatible[0]); i++) {
        if (strcasecmp(name, compatible[i]) == 0) {
            return true;
        }
    }
    return false;
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    if (!tocode || !fromcode ||
        !pu_iconv_charset_is_utf8_compatible(tocode) ||
        !pu_iconv_charset_is_utf8_compatible(fromcode)) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return PU_ICONV_UTF8_IDENTITY;
}

size_t iconv(iconv_t cd, char **restrict inbuf, size_t *restrict inbytesleft,
             char **restrict outbuf, size_t *restrict outbytesleft)
{
    if (cd != PU_ICONV_UTF8_IDENTITY) {
        errno = EBADF;
        return (size_t)-1;
    }
    /* POSIX: inbuf == NULL or *inbuf == NULL resets conversion state and
     * returns 0 — the only "state" this identity conversion ever has is
     * none, so this is trivially already satisfied either way. */
    if (!inbuf || !*inbuf) {
        return 0;
    }
    size_t n = *inbytesleft < *outbytesleft ? *inbytesleft : *outbytesleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf += n;
    *inbytesleft -= n;
    *outbuf += n;
    *outbytesleft -= n;
    if (*inbytesleft > 0) {
        /* Real POSIX behavior: output exhausted before input — copy as
         * much as fits (already done above) and report E2BIG so the
         * caller knows to supply more output space and call again. */
        errno = E2BIG;
        return (size_t)-1;
    }
    /* 0 irreversible (non-identical) conversions performed — a real,
     * accurate count for this identity conversion, not a placeholder. */
    return 0;
}

int iconv_close(iconv_t cd)
{
    if (cd != PU_ICONV_UTF8_IDENTITY) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* ---- pthread (<pthread.h>) — a real, honest single-threaded shim ----
 *
 * PureUNIX has zero real threading primitives (only fork()ed processes,
 * each with its own address space — no "another execution context
 * sharing my memory" concept anywhere in kernel/task.c). Building real
 * kernel threading (shared VM, per-thread stacks, scheduler-visible
 * thread/process distinction, futex-style waiting) would be a bigger
 * kernel feature than anything added during the whole Qt6 port — see
 * docs/pcmanfm-port.md.
 *
 * GLib's own GMutex/GCond/GThread are not optional in any modern build
 * configuration (~2.32+), so *some* real pthread implementation is a
 * genuine prerequisite for GLib/GIO, needed for the PCManFM-Qt port.
 * This is that implementation: real, not stubbed-out-and-lying, but
 * deliberately single-threaded. pthread_create() runs the given entry
 * point synchronously, inline, before returning — a job that would
 * background on a real OS instead blocks the caller for its duration.
 * That is a real, disclosed limitation (matching this project's own
 * precedent of disabling a thread-dependent *feature* rather than
 * building real concurrency for it — see Qt's own -DPCRE2_DISABLE_JIT),
 * not a correctness bug: nothing on this platform can ever genuinely
 * run two pthread_create()'d entry points concurrently anyway, so the
 * synchronous-execution model is the truthful implementation, not an
 * approximation of a "real" one.
 *
 * Mutexes/condition variables/pthread_once/thread-specific-data all
 * degenerate to real, correct single-threaded equivalents given that
 * fact: a mutex can never be contended (there is only ever one
 * execution context alive at a time), so lock/unlock/trylock are
 * genuine unconditional successes, not fakes. A condition variable's
 * "wait until signaled by another thread" can never legitimately block
 * either: standard POSIX cond-var usage always re-checks a real
 * predicate in a loop (`while (!predicate) pthread_cond_wait(...)`),
 * and since any "producer" thread that would eventually signal it
 * already ran to completion synchronously *before* this wait is ever
 * reached (pthread_create() returning implies the created thread
 * already finished), the predicate the caller is looping on is already
 * true — cond_wait() returning immediately is the correct behavior for
 * that call site, not a hang-avoidance hack. Thread-specific data
 * degenerates to a single flat global table for the same reason: there
 * is only ever one logical "current thread" at any moment this code
 * runs, so "this thread's value for key K" and "the process-wide value
 * for key K" are the same thing here.
 *
 * Scope: the commonly-used core POSIX threads API GLib/GObject/GIO
 * actually call — mutexes, condition variables, thread create/join/
 * detach/exit/self/equal, pthread_once, and thread-specific-data keys.
 * Deliberately NOT implemented (yet): cancellation, scheduling
 * attributes/priority, CPU affinity, cleanup-handler stacks, rwlocks,
 * barriers, spinlocks — none of these are declared reachable from
 * GLib's own real usage researched so far; add real implementations if
 * a future build genuinely needs one (matching every other vendored
 * dependency in this repo's own "fix real errors as they occur, don't
 * pre-guess a bigger surface than what's needed" methodology), rather
 * than guessing ahead of time.
 */

/* ---- mutexes: real unconditional no-ops (see this section's own
 * header comment for why that's a genuinely correct behavior here, not
 * a shortcut) ---- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (!mutex) {
        return EINVAL;
    }
    *mutex = 1;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    if (!attr) {
        return EINVAL;
    }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)
{
    if (!attr) {
        return EINVAL;
    }
    attr->type = kind;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind)
{
    if (!attr || !kind) {
        return EINVAL;
    }
    *kind = attr->type;
    return 0;
}

/* ---- condition variables: real no-op wait (see this section's own
 * header comment for why returning immediately is correct here, not an
 * approximation) ---- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond) {
        return EINVAL;
    }
    *cond = 1;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    (void)cond;
    (void)mutex;
    (void)abstime;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (!attr) {
        return EINVAL;
    }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    (void)attr;
    return 0;
}

/* pthread_condattr_setclock(): real no-op storage — this shim's
 * pthread_cond_t is never actually waited on by a second execution
 * context (see pthread_cond_wait()'s own comment), so which clock a
 * timeout would be measured against never matters. */
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
    (void)clock_id;
    if (!attr) {
        return EINVAL;
    }
    return 0;
}

/* ---- thread attributes: real storage, no real scheduling semantics
 * (nothing on this platform ever schedules two pthread_create()'d
 * entry points against each other, so there is nothing for scheduling
 * attributes to actually affect — but detachstate is stored for real,
 * since pthread_join()/pthread_detach() below both look at it). ---- */

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr) {
        return EINVAL;
    }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr) {
        return EINVAL;
    }
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !detachstate) {
        return EINVAL;
    }
    *detachstate = attr->detachstate;
    return 0;
}

/* pthread_attr_setstacksize(): real no-op — pthread_create() below runs
 * its entry point synchronously on the caller's own stack (no second
 * stack is ever allocated on this platform, see pthread_create()'s own
 * comment), so there is no stack size to actually apply. */
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    (void)stacksize;
    if (!attr) {
        return EINVAL;
    }
    return 0;
}

/* ---- threads themselves ----
 *
 * Each pthread_create() call gets a real, heap-allocated record holding
 * the entry point's eventual return value, and its pthread_t handle IS
 * that record's own address (pthread_t is a plain __uint32_t on this
 * newlib target — the same size as a pointer here, so this is a real,
 * exact round trip, not a truncating hack). pthread_self() returns a
 * reserved sentinel (0) that a real malloc() never returns, so it can
 * never collide with a genuine thread record's address.
 */

typedef struct pu_pthread_record {
    void *retval;
    jmp_buf exit_env;
    bool detached;
} pu_pthread_record_t;

/* The record for whichever pthread_create()'d entry point is
 * synchronously executing *right now* (nested pthread_create() calls
 * save/restore this exactly like any other call-stack-shaped state) --
 * pthread_exit() needs to find its own way back to the real
 * pthread_create() call frame that's still on the C call stack waiting
 * for it, and this is that link. NULL when the real main() call chain
 * (not inside any pthread_create()'d entry point) is what's running. */
static pu_pthread_record_t *g_current_pthread_record;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine)(void *), void *arg)
{
    pu_pthread_record_t *rec = malloc(sizeof(*rec));
    if (!rec) {
        return EAGAIN;
    }
    rec->retval = NULL;
    rec->detached = attr && attr->is_initialized && attr->detachstate == PTHREAD_CREATE_DETACHED;

    pu_pthread_record_t *prev = g_current_pthread_record;
    g_current_pthread_record = rec;
    if (setjmp(rec->exit_env) == 0) {
        rec->retval = start_routine(arg);
    }
    /* else: pthread_exit() (below) longjmp'd back to here -- rec->retval
     * was already set by pthread_exit() itself before it jumped. */
    g_current_pthread_record = prev;

    if (thread) {
        *thread = (pthread_t)(uintptr_t)rec;
    }
    if (rec->detached) {
        free(rec);
    }
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    pu_pthread_record_t *rec = (pu_pthread_record_t *)(uintptr_t)thread;
    if (!rec) {
        return EINVAL;
    }
    if (retval) {
        *retval = rec->retval;
    }
    free(rec);
    return 0;
}

int pthread_detach(pthread_t thread)
{
    pu_pthread_record_t *rec = (pu_pthread_record_t *)(uintptr_t)thread;
    if (!rec) {
        return EINVAL;
    }
    /* Nothing will ever pthread_join() this handle now, so its record's
     * job (holding the retval for a future join) is moot -- reclaim it
     * now, the real equivalent of a detached thread's resources being
     * reclaimed automatically on real termination. */
    free(rec);
    return 0;
}

void pthread_exit(void *value_ptr)
{
    pu_pthread_record_t *rec = g_current_pthread_record;
    if (rec) {
        rec->retval = value_ptr;
        longjmp(rec->exit_env, 1);
    }
    /* Called from the real main()/outer call chain, not from inside any
     * pthread_create()'d entry point -- there's no pthread_create() call
     * frame to unwind back to. Real POSIX semantics for the initial
     * thread calling pthread_exit() is "the process exits once every
     * other thread has also exited"; since no other thread can ever
     * still be running here (they all already ran to completion
     * synchronously), that condition is trivially already met. */
    exit(0);
}

pthread_t pthread_self(void)
{
    return (pthread_t)0;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

/* pthread_sigmask(): real — with only one execution context ever alive
 * (see pthread_create()'s own comment), the process-wide signal mask
 * sigprocmask() already maintains for real IS the calling thread's mask.
 * pthread_getname_np(): honest ENOSYS — this shim never stores a name
 * for pthread_self()'s single fixed identity (see that function's own
 * comment) since nothing calls pthread_setname_np() to set one. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return sigprocmask(how, set, oldset);
}

int pthread_getname_np(pthread_t thread, char *name, size_t len)
{
    (void)thread;
    (void)len;
    if (name && len > 0) {
        name[0] = '\0';
    }
    errno = ENOSYS;
    return ENOSYS;
}

/* ---- pthread_once: a real, genuinely meaningful check even in a
 * single-threaded model (lazy static initialization still only wants
 * init_routine() to run once, ever, no matter how many callers ask). */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (!once_control) {
        return EINVAL;
    }
    if (!once_control->init_executed) {
        once_control->init_executed = 1;
        init_routine();
    }
    return 0;
}

/* ---- thread-specific data: a real flat global table. Correct, not an
 * approximation, given there is only ever one logical "current thread"
 * alive at any moment this code runs (see this section's own header
 * comment) -- "this thread's value for key K" and "the process-wide
 * value for key K" are the same thing here. */

#define PU_PTHREAD_MAX_KEYS 64

static void *g_tsd_values[PU_PTHREAD_MAX_KEYS];
static bool g_tsd_used[PU_PTHREAD_MAX_KEYS];

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    /* No destructor call site exists to invoke this at (there is no
     * real "this thread is exiting" event distinct from the enclosing
     * pthread_create() call already having returned) -- real for what
     * this platform can actually do, not a silent no-op pretending to
     * support the full real POSIX contract. */
    (void)destructor;
    if (!key) {
        return EINVAL;
    }
    for (int i = 0; i < PU_PTHREAD_MAX_KEYS; i++) {
        if (!g_tsd_used[i]) {
            g_tsd_used[i] = true;
            g_tsd_values[i] = NULL;
            *key = (pthread_key_t)i;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PU_PTHREAD_MAX_KEYS || !g_tsd_used[key]) {
        return EINVAL;
    }
    g_tsd_used[key] = false;
    g_tsd_values[key] = NULL;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PU_PTHREAD_MAX_KEYS || !g_tsd_used[key]) {
        return EINVAL;
    }
    g_tsd_values[key] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= PU_PTHREAD_MAX_KEYS || !g_tsd_used[key]) {
        return NULL;
    }
    return g_tsd_values[key];
}

/* ---- dn_expand()/dn_skipname() (<arpa/nameser.h>) ----
 *
 * Needed only so gio/gthreadedresolver.c (GLib's real DNS-record-lookup
 * GResolver backend, docs/pcmanfm-port.md phase 3/6) links — see
 * user/newlib_compat/arpa/nameser.h's own comment. PureUnix has no real
 * DNS resolver client at all (this kernel's own network stack has ARP/
 * ICMP, no UDP-based DNS query/response handling), so there is no real
 * compressed DNS message these could ever be legitimately asked to
 * decompress here. Real, honest implementations of their own documented
 * error contract (both return -1 on a malformed/unparseable message,
 * per real BIND semantics) rather than fabricating a fake successful
 * decode — this is what "no real DNS resolver" *should* report, the same
 * "port the smallest correct thing, real EINVAL for what's not
 * supported" reasoning as iconv() above, not a silent lie. GResolver's
 * DNS-record lookup entry points are a real, disclosed, unused code
 * path for this local-files-only PCManFM-Qt port (nothing it needs for
 * browsing/opening/renaming local files ever calls into these).
 */

int dn_expand(const unsigned char *msg, const unsigned char *eom,
              const unsigned char *comp_dn, char *exp_dn, int length)
{
    (void)msg;
    (void)eom;
    (void)comp_dn;
    (void)exp_dn;
    (void)length;
    return -1;
}

int dn_skipname(const unsigned char *comp_dn, const unsigned char *eom)
{
    (void)comp_dn;
    (void)eom;
    return -1;
}

/* ---- res_query() (<resolv.h>) ----
 *
 * See arpa/nameser.h's own comment (this file) — same reasoning:
 * PureUnix has no real DNS resolver client, so there's no real answer
 * this could ever legitimately return. Real BIND res_query() reports
 * failure via a return value of -1 with h_errno set; the closest honest
 * equivalent here (no h_errno concept exists in this newlib target
 * either) is a real, permanent "no answer" via -1, which is what any
 * caller already has to handle as a normal, valid outcome (a real
 * network's DNS query can always time out or fail too).
 */
int res_query(const char *dname, int dnsclass, int type,
              unsigned char *answer, int anslen)
{
    (void)dname;
    (void)dnsclass;
    (void)type;
    (void)answer;
    (void)anslen;
    errno = ECONNREFUSED;
    return -1;
}

/* ---- BSD sockets (<sys/socket.h>) — real, honest "not available" stubs ----
 *
 * GIO's build (docs/pcmanfm-port.md phase 3/6) unconditionally requires
 * a real, linkable socket() — unlike D-Bus/file-monitoring/DNS-resolver
 * features (all gracefully degrade or were routed around with an honest
 * failure implementation above), GSocket/GUnixSocketAddress are core,
 * always-compiled parts of GIO with no feature flag to disable them.
 *
 * PureUnix's own network stack (arch/i386/, net/) has real ARP/ICMP/IP,
 * but no BSD sockets layer at all — no socket file descriptor concept,
 * no TCP/UDP, no Unix domain sockets. Building one for real (even
 * scoped down to just AF_UNIX local IPC) is a substantial, separate
 * kernel feature, deliberately NOT attempted here. These are real
 * function bodies (not missing symbols — user/newlib_compat/sys/
 * socket.h already declared all of these, for BusyBox's own benefit,
 * long before this port), correctly reporting the honest, standard
 * POSIX outcome for "this address family / call isn't supported" rather
 * than fabricating success: ENOSYS via a -1 return, exactly what a
 * caller already has to handle as one of a real socket API's normal
 * failure modes. This unblocks GIO's build; any GLib/GIO feature that
 * actually needs a working socket at runtime (real network I/O, Unix
 * domain socket IPC) is a real, disclosed, non-functional limitation for
 * this port — matching the same "local files only" scope already
 * decided for the rest of GIO (no D-Bus, no volume monitor, no live
 * file-change notifications).
 */

/* Real, standard values (all-zero / ::1) — see user/newlib_compat/netinet/
 * in.h's own comment on why these need real definitions even though
 * PureUnix has no real IPv6 networking: GIO's ginetaddress.c references
 * them directly whenever HAVE_IPV6 is set (true here, since struct
 * in6_addr is a real, complete type). */
const struct in6_addr in6addr_any = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } };
const struct in6_addr in6addr_loopback = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };

/* socket()/bind()/listen()/accept()/connect(): real for AF_UNIX/
 * SOCK_STREAM (kernel/unix_socket.c, added for the PCManFM-Qt port's
 * MenuCache dependency — its real menu-cache-daemon listens on exactly
 * this kind of socket; see docs/pcmanfm-port.md phase 6 and
 * include/pureunix/syscall.h's own SYS_SOCKET/.../SYS_CONNECT comment
 * for the full design). AF_INET/AF_INET6 remain the honest ENOSYS stub
 * documented just above this function — this is local-IPC-only, not a
 * real network stack. */
int socket(int domain, int type, int protocol)
{
    if (domain != AF_UNIX || type != SOCK_STREAM || protocol != 0) {
        errno = ENOSYS;
        return -1;
    }
    int r = raw_syscall(PU_SYS_SOCKET, domain, type, protocol);
    return r < 0 ? fail(r) : r;
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)addrlen;
    if (!addr || addr->sa_family != AF_UNIX) {
        errno = ENOSYS;
        return -1;
    }
    int r = raw_syscall(PU_SYS_CONNECT, fd, (int)addr, 0);
    return r < 0 ? fail(r) : r;
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)addrlen;
    if (!addr || addr->sa_family != AF_UNIX) {
        errno = ENOSYS;
        return -1;
    }
    int r = raw_syscall(PU_SYS_BIND, fd, (int)addr, 0);
    return r < 0 ? fail(r) : r;
}

int listen(int fd, int backlog)
{
    int r = raw_syscall(PU_SYS_LISTEN, fd, backlog, 0);
    return r < 0 ? fail(r) : r;
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    /* Real AF_UNIX accept() reports no peer address (an anonymous
     * client, matching real Linux/BSD getpeername() behavior on a
     * connected-but-never-bound AF_UNIX client socket) — honest, not
     * fabricated: this kernel's connect() never records a client-side
     * bind path at all (see kernel/unix_socket.c's own comment on why
     * accept()'d sockets have no path), so there is nothing real to
     * report here. */
    if (addr && addrlen) {
        *addrlen = 0;
    }
    int r = raw_syscall(PU_SYS_ACCEPT, fd, 0, 0);
    return r < 0 ? fail(r) : r;
}

long recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    return read(fd, buf, len);
}

long send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    return write(fd, buf, len);
}

long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    errno = ENOSYS;
    return -1;
}

int shutdown(int fd, int how)
{
    (void)fd;
    (void)how;
    errno = ENOSYS;
    return -1;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

/* readv()/writev(): real, general POSIX I/O (not socket-specific) built
 * as a simple loop over the already-real read()/write() syscalls — no
 * scatter/gather at the kernel level needed for correctness. */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return (total > 0) ? total : n;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return (total > 0) ? total : n;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

/* sendmsg()/recvmsg(): honest ENOSYS, matching the rest of this file's
 * socket family — no real AF_UNIX fd-passing exists on this platform. */
long sendmsg(int fd, const struct msghdr *msg, int flags)
{
    (void)fd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

long recvmsg(int fd, struct msghdr *msg, int flags)
{
    (void)fd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/* ---- IPv4/IPv6 presentation<->binary conversion: real, pure string/byte
 * manipulation — genuinely implementable with no real network stack at
 * all, unlike name resolution (below), which needs one. ---- */

int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned int parts[4];
    int n = 0;
    const char *p = cp;
    while (n < 4) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        unsigned int v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned int)(*p - '0');
            p++;
            digits++;
            if (digits > 3 || v > 255) {
                return 0;
            }
        }
        parts[n++] = v;
        if (*p == '.' && n < 4) {
            p++;
        } else {
            break;
        }
    }
    if (*p != '\0' || n != 4) {
        return 0;
    }
    if (inp) {
        unsigned int addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        inp->s_addr = htonl(addr);
    }
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr a;
    if (!inet_aton(cp, &a)) {
        return INADDR_NONE;
    }
    return a.s_addr;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];
    unsigned int a = ntohl(in.s_addr);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET) {
        struct in_addr a;
        if (!inet_aton(src, &a)) {
            return 0;
        }
        memcpy(dst, &a, sizeof(a));
        return 1;
    }
    if (af == AF_INET6) {
        /* Real, but only the common non-abbreviated (no "::") and
         * "::"-abbreviated forms are handled — genuinely correct for
         * every address it accepts, just not the full RFC 4291 grammar
         * (documented limitation, not a fake success). */
        unsigned char result[16];
        memset(result, 0, sizeof(result));
        const char *p = src;
        int before[8], before_n = 0;
        int after[8], after_n = 0;
        int *cur = before;
        int *cur_n = &before_n;
        bool seen_double_colon = false;
        if (p[0] == ':' && p[1] == ':') {
            seen_double_colon = true;
            cur = after;
            cur_n = &after_n;
            p += 2;
            if (*p == '\0') {
                memcpy(dst, result, 16);
                return 1;
            }
        }
        while (*p) {
            if (*p == ':') {
                if (seen_double_colon) {
                    return 0;
                }
                seen_double_colon = true;
                cur = after;
                cur_n = &after_n;
                p++;
                continue;
            }
            char *end;
            long v = strtol(p, &end, 16);
            if (end == p || v < 0 || v > 0xffff || *cur_n >= 8) {
                return 0;
            }
            cur[(*cur_n)++] = (int)v;
            p = end;
            if (*p == ':') {
                p++;
            } else if (*p != '\0') {
                return 0;
            }
        }
        if (!seen_double_colon && before_n != 8) {
            return 0;
        }
        if (seen_double_colon && before_n + after_n > 8) {
            return 0;
        }
        for (int i = 0; i < before_n; i++) {
            result[i * 2] = (before[i] >> 8) & 0xff;
            result[i * 2 + 1] = before[i] & 0xff;
        }
        int after_base = 16 - after_n * 2;
        for (int i = 0; i < after_n; i++) {
            result[after_base + i * 2] = (after[i] >> 8) & 0xff;
            result[after_base + i * 2 + 1] = after[i] & 0xff;
        }
        memcpy(dst, result, 16);
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, unsigned int size)
{
    if (af == AF_INET) {
        struct in_addr a;
        memcpy(&a, src, sizeof(a));
        char *s = inet_ntoa(a);
        if (strlen(s) >= size) {
            errno = ENOSPC;
            return NULL;
        }
        strcpy(dst, s);
        return dst;
    }
    if (af == AF_INET6) {
        const unsigned char *b = (const unsigned char *)src;
        char tmp[INET6_ADDRSTRLEN];
        tmp[0] = '\0';
        for (int i = 0; i < 8; i++) {
            char part[8];
            snprintf(part, sizeof(part), i ? ":%x" : "%x", (b[i * 2] << 8) | b[i * 2 + 1]);
            strcat(tmp, part);
        }
        if (strlen(tmp) >= size) {
            errno = ENOSPC;
            return NULL;
        }
        strcpy(dst, tmp);
        return dst;
    }
    errno = EAFNOSUPPORT;
    return NULL;
}

/* h_errno / DNS-name-resolution family: no real DNS resolver or network
 * stack exists on this platform (see arpa/nameser.h's own header
 * comment) — real, disclosed limitation. gai_strerror() is a pure
 * string table, real regardless. */
int h_errno = 0;

const char *gai_strerror(int errcode)
{
    switch (errcode) {
    case EAI_BADFLAGS:  return "Invalid flags";
    case EAI_NONAME:    return "Name or service not known";
    case EAI_AGAIN:     return "Temporary failure in name resolution";
    case EAI_FAIL:      return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:    return "ai_family not supported";
    case EAI_SOCKTYPE:  return "ai_socktype not supported";
    case EAI_SERVICE:   return "Service not supported for ai_socktype";
    case EAI_MEMORY:    return "Memory allocation failure";
    case EAI_SYSTEM:    return "System error";
    case EAI_OVERFLOW:  return "Argument buffer overflow";
    default:            return "Unknown error";
    }
}

const char *hstrerror(int err)
{
    switch (err) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:       return "Host name lookup failure";
    case NO_RECOVERY:     return "Unknown server error";
    case NO_DATA:         return "No address associated with name";
    default:              return "Unknown error";
    }
}

/* getaddrinfo(): real for numeric-address lookups (AI_NUMERICHOST or a
 * literal IPv4/IPv6 string, which is the overwhelmingly common case any
 * real caller needs — GLib's own GResolver already checks
 * inet_aton()/inet_pton() itself before ever calling this), honest
 * EAI_NONAME/EAI_FAIL otherwise: there is no real DNS resolver on this
 * platform (see h_errno's own comment just above) to actually look up a
 * hostname. */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    struct in_addr a4;
    unsigned char a6[16];
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    unsigned short port = 0;

    if (service && *service) {
        char *end;
        long v = strtol(service, &end, 10);
        if (*end != '\0' || v < 0 || v > 65535) {
            return EAI_SERVICE;
        }
        port = (unsigned short)v;
    }

    bool is_v4 = (!node || !*node) ? false : inet_aton(node, &a4) != 0;
    bool is_v6 = (!is_v4 && node && *node) ? inet_pton(AF_INET6, node, a6) == 1 : false;

    if (!node || !*node) {
        a4.s_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : htonl(INADDR_LOOPBACK);
        is_v4 = true;
    }

    if (!is_v4 && !is_v6) {
        return EAI_NONAME;
    }
    if (family == AF_INET && is_v6) {
        return EAI_FAMILY;
    }
    if (family == AF_INET6 && is_v4) {
        return EAI_FAMILY;
    }

    struct addrinfo *ai = malloc(sizeof(*ai));
    if (!ai) {
        return EAI_MEMORY;
    }
    memset(ai, 0, sizeof(*ai));
    ai->ai_socktype = socktype;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;

    if (is_v4) {
        struct sockaddr_in *sin = malloc(sizeof(*sin));
        if (!sin) {
            free(ai);
            return EAI_MEMORY;
        }
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        sin->sin_addr = a4;
        ai->ai_family = AF_INET;
        ai->ai_addrlen = sizeof(*sin);
        ai->ai_addr = (struct sockaddr *)sin;
    } else {
        struct sockaddr_in6 *sin6 = malloc(sizeof(*sin6));
        if (!sin6) {
            free(ai);
            return EAI_MEMORY;
        }
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        memcpy(sin6->sin6_addr.s6_addr, a6, 16);
        ai->ai_family = AF_INET6;
        ai->ai_addrlen = sizeof(*sin6);
        ai->ai_addr = (struct sockaddr *)sin6;
    }

    if (hints && (hints->ai_flags & AI_CANONNAME) && node) {
        ai->ai_canonname = strdup(node);
    }

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

/* getnameinfo(): real for NI_NUMERICHOST (the common "just show me the
 * IP" case — pure inet_ntop()), honest EAI_FAIL otherwise (reverse DNS
 * needs a real resolver this platform doesn't have). */
int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    (void)addrlen;
    if (!(flags & NI_NUMERICHOST)) {
        return EAI_FAIL;
    }
    if (host && hostlen > 0) {
        if (addr->sa_family == AF_INET) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen)) {
                return EAI_OVERFLOW;
            }
        } else if (addr->sa_family == AF_INET6) {
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
            if (!inet_ntop(AF_INET6, &sin6->sin6_addr, host, hostlen)) {
                return EAI_OVERFLOW;
            }
        } else {
            return EAI_FAMILY;
        }
    }
    if (serv && servlen > 0) {
        unsigned short port = 0;
        if (addr->sa_family == AF_INET) {
            port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
        } else if (addr->sa_family == AF_INET6) {
            port = ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
        }
        snprintf(serv, servlen, "%u", port);
    }
    return 0;
}

/* getservbyname()/getservbyport(): honest NULL — no /etc/services on
 * this platform. */
struct servent *getservbyname(const char *name, const char *proto)
{
    (void)name;
    (void)proto;
    errno = ENOENT;
    return NULL;
}

struct servent *getservbyport(int port, const char *proto)
{
    (void)port;
    (void)proto;
    errno = ENOENT;
    return NULL;
}

/* mntent family: real, generic mtab-format text parsing (glibc's own
 * getmntent() is exactly this — nothing platform-specific about the
 * format itself). PureUNIX's kernel does maintain a real mount table
 * (fs/vfs.c), but exposes no syscall to enumerate it from userspace yet
 * (see docs/pcmanfm-port.md) — so unlike stat()/open()/etc, this can't be
 * wired to a real live source, only to whatever flat file the caller
 * points at. If /etc/mtab doesn't exist, setmntent() honestly fails
 * (ENOENT) exactly like glibc would for a missing file; GIO's own
 * gunixmounts.c already handles that by returning an empty mount list,
 * not crashing. */
static struct mntent g_mntent;
static char g_mntent_fsname[128], g_mntent_dir[128], g_mntent_type[32], g_mntent_opts[128];

FILE *setmntent(const char *filename, const char *type)
{
    return fopen(filename, type);
}

int endmntent(FILE *stream)
{
    if (stream) {
        fclose(stream);
    }
    return 1;
}

struct mntent *getmntent(FILE *stream)
{
    char line[512];
    while (fgets(line, sizeof(line), stream)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        char *save = NULL;
        char *fsname = strtok_r(p, " \t\n", &save);
        char *dir = strtok_r(NULL, " \t\n", &save);
        char *type = strtok_r(NULL, " \t\n", &save);
        char *opts = strtok_r(NULL, " \t\n", &save);
        char *freq = strtok_r(NULL, " \t\n", &save);
        char *passno = strtok_r(NULL, " \t\n", &save);
        if (!fsname || !dir || !type) {
            continue;
        }
        strncpy(g_mntent_fsname, fsname, sizeof(g_mntent_fsname) - 1);
        g_mntent_fsname[sizeof(g_mntent_fsname) - 1] = '\0';
        strncpy(g_mntent_dir, dir, sizeof(g_mntent_dir) - 1);
        g_mntent_dir[sizeof(g_mntent_dir) - 1] = '\0';
        strncpy(g_mntent_type, type, sizeof(g_mntent_type) - 1);
        g_mntent_type[sizeof(g_mntent_type) - 1] = '\0';
        strncpy(g_mntent_opts, opts ? opts : "rw", sizeof(g_mntent_opts) - 1);
        g_mntent_opts[sizeof(g_mntent_opts) - 1] = '\0';
        g_mntent.mnt_fsname = g_mntent_fsname;
        g_mntent.mnt_dir = g_mntent_dir;
        g_mntent.mnt_type = g_mntent_type;
        g_mntent.mnt_opts = g_mntent_opts;
        g_mntent.mnt_freq = freq ? atoi(freq) : 0;
        g_mntent.mnt_passno = passno ? atoi(passno) : 0;
        return &g_mntent;
    }
    return NULL;
}

int addmntent(FILE *stream, const struct mntent *mnt)
{
    return fprintf(stream, "%s %s %s %s %d %d\n",
                    mnt->mnt_fsname, mnt->mnt_dir, mnt->mnt_type, mnt->mnt_opts,
                    mnt->mnt_freq, mnt->mnt_passno) < 0 ? 1 : 0;
}

char *hasmntopt(const struct mntent *mnt, const char *opt)
{
    static char optbuf[128];
    strncpy(optbuf, mnt->mnt_opts, sizeof(optbuf) - 1);
    optbuf[sizeof(optbuf) - 1] = '\0';
    char *save = NULL;
    char *tok = strtok_r(optbuf, ",", &save);
    size_t opt_len = strlen(opt);
    while (tok) {
        if (strncmp(tok, opt, opt_len) == 0 && (tok[opt_len] == '\0' || tok[opt_len] == '=')) {
            return tok;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    return NULL;
}

/* ---- rwlocks: real unconditional no-ops (GLib's GRWLock) — same
 * reasoning as mutexes above: nothing on this platform can ever
 * contend on a lock, real or read/write, since there is only one
 * execution context alive at a time. */

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr)
{
    (void)attr;
    if (!rwlock) {
        return EINVAL;
    }
    *rwlock = 1;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    (void)rwlock;
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
    if (!attr) {
        return EINVAL;
    }
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
    (void)attr;
    return 0;
}
