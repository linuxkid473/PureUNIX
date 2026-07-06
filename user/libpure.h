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
#define SYS_FORK     23
#define SYS_EXEC     24
#define SYS_WAIT     25
#define SYS_TCGETATTR 26
#define SYS_TCSETATTR 27
#define SYS_IOCTL     28
#define SYS_CHDIR     29
#define SYS_GETCWD    30
#define SYS_PIPE      36
#define SYS_DUP       37
#define SYS_DUP2      38
#define SYS_KILL      39

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
#define ESRCH   (-3)
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
#define EINTR   (-4)
#define ENOTTY  (-25)
#define ERANGE  (-34)
#define EPIPE   (-32)

/* Signal numbers for pu_kill() — must match include/pureunix/signal.h
 * (kernel-side) and newlib's <signal.h> (real POSIX numbering for this
 * target, see that file's own header comment). */
#define SIGKILL 9
#define SIGTERM 15

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

/* Terminal control — must match include/pureunix/termios.h. */
#define NCCS 8

#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VMIN   5
#define VTIME  6
#define VSUSP  7

#define ICRNL  0x0001
#define INLCR  0x0002

#define OPOST  0x0001
#define ONLCR  0x0002

#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0004
#define ECHOE  0x0008
#define ECHOK  0x0010
#define ECHONL 0x0020

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* ioctl() — must match include/pureunix/ioctl.h. */
#define TIOCGWINSZ 1

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

/* Raw syscall gate, for probing a syscall directly rather than through one
 * of the named wrappers below (e.g. an unrecognized syscall number's -1
 * return, or SYS_EXIT's pass-through return value with the current task
 * left running — see docs/syscalls.md's SYS_EXIT section). */
int    pu_syscall_raw(int n, int a, int b, int c);
int    pu_getpid(void);
int    pu_yield(void);

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
/* Changes this task's working directory to path (resolved relative to its
 * current one). Fails with -ENOTDIR if path isn't a directory, -EACCES
 * without X_OK on it, or -ENAMETOOLONG if the resolved path doesn't fit. */
int    pu_chdir(const char *path);
/* Copies this task's absolute working directory into buf (size bytes,
 * NUL-terminated). Fails with -ERANGE if it doesn't fit. */
int    pu_getcwd(char *buf, size_t size);

/* Creates a pipe: fds[0] becomes the read end, fds[1] the write end.
 * Returns 0 on success, or a negative error code (-EMFILE if no two fds
 * are free). */
int    pu_pipe(int fds[2]);
/* Duplicates oldfd onto the lowest-numbered free fd, sharing the same
 * open file description (offset included) — dup()/fork() do the same
 * sharing internally; see include/pureunix/task.h's open_file_t.
 * Returns the new fd, or a negative error code. */
int    pu_dup(int oldfd);
/* Same as pu_dup(), but the caller picks newfd — if it was already open,
 * it's closed first. A no-op returning newfd if oldfd == newfd. */
int    pu_dup2(int oldfd, int newfd);
/* Terminates task pid with signal sig's POSIX default action (there's no
 * handler-dispatch mechanism — see docs/syscalls.md's SYS_KILL section —
 * so every nonzero sig just kills the target outright); sig == 0 is
 * POSIX's "null signal", probing whether pid exists without killing it.
 * If pid is the caller's own, this never returns on success (the kernel
 * stops the caller directly, same as pu_exit()). Returns 0 on success, or
 * -EINVAL (pid <= 0 — no process-group support) / -ESRCH (no such pid). */
int    pu_kill(int pid, int sig);

/* -------------------------------------------------------------------- */
/* Per-process address spaces: fork / exec / wait                        */
/* -------------------------------------------------------------------- */

/* Duplicates the calling process: returns the child's pid (> 0) in the
 * parent, 0 in the child, or a negative value on failure. Parent and
 * child have independent copies of memory from this point on. */
int    pu_fork(void);
/* Replaces the calling process's own memory image with the program at
 * path, in place. Only returns (with a negative error code) on failure —
 * on success control never comes back here. Equivalent to
 * pu_execve(path, NULL, NULL). */
int    pu_exec(const char *path);
/* Same as pu_exec(), but argv is copied onto the new process's stack (a
 * NULL-terminated array — NULL here means the same as pu_exec(): the new
 * process sees a bare argv = {path, NULL}), and envp similarly becomes the
 * new process's environ (NULL means an empty environment). Both arrays and
 * every string they point to are read from *this* process's memory before
 * the new image replaces it, so they don't need to (and can't) survive the
 * call themselves. */
int    pu_execve(const char *path, char *const argv[], char *const envp[]);
/* Blocks until the child identified by pid (or, if pid == -1, any child)
 * exits, then reaps it. Returns the reaped child's pid, or a negative
 * value if the caller has no such child. If status is non-NULL, the
 * child's exit code is written there. */
int    pu_wait(int pid, int *status);
/* Immediately terminates the calling process with the given exit code —
 * unlike SYS_EXIT (see pu_syscall_raw's doc comment), this does not
 * return. Equivalent to what crt0.S does after main() returns, but
 * callable mid-program (e.g. by a fork()ed child that must not fall
 * through to the parent's remaining code). */
void   pu_exit(int code) __attribute__((noreturn));

/* -------------------------------------------------------------------- */
/* Terminal control                                                       */
/* -------------------------------------------------------------------- */

/* fd must be 0, 1, or 2 (stdin/stdout/stderr all name the same console);
 * any other fd fails with -ENOTTY (open, but not a terminal) or -EBADF
 * (not a valid/open descriptor at all). */
int    pu_tcgetattr(int fd, struct termios *out);
int    pu_tcsetattr(int fd, int actions, const struct termios *in);
/* Generic device control. The only supported request is TIOCGWINSZ, which
 * fills *argp (a struct winsize *) with the console's fixed 80x25 size.
 * Same fd/error contract as pu_tcgetattr above, plus -EINVAL for a request
 * other than TIOCGWINSZ or a null argp. */
int    pu_ioctl(int fd, int request, void *argp);
/* POSIX isatty(): 1 if fd names the console (0, 1, or 2), 0 otherwise —
 * including a bad or non-terminal fd. Implemented as pu_tcgetattr()
 * succeeding, exactly like isatty() on a real UNIX is ioctl(TCGETS)
 * succeeding; there is no dedicated syscall for it. */
int    pu_isatty(int fd);

void   pu_puts(const char *s);
void   pu_puti(int value);
size_t pu_strlen(const char *s);
int    pu_atoi(const char *s);

#endif
