#include <pureunix/arch.h>
#include <pureunix/dirent.h>
#include <pureunix/elf.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/flock.h>
#include <pureunix/framebuffer.h>
#include <pureunix/icmp.h>
#include <pureunix/input.h>
#include <pureunix/ioctl.h>
#include <pureunix/memory.h>
#include <pureunix/pty.h>
#include <pureunix/signal.h>
#include <pureunix/stat.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/time.h>
#include <pureunix/tty.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>
#include <pureunix/vmm.h>
#include <pureunix/vt.h>
#include <pureunix/wait.h>

/* fcntl() commands — same numbering as newlib's <sys/_default_fcntl.h> (and
 * Linux), so user/newlib_syscalls.c's fcntl() can pass cmd straight through
 * unmodified; only the O_* flag bits themselves need translating (same as
 * open(), see that file). */
enum {
    PU_F_DUPFD = 0,
    PU_F_GETFD = 1,
    PU_F_SETFD = 2,
    PU_F_GETFL = 3,
    PU_F_SETFL = 4,
    PU_F_GETLK = 7,
    PU_F_SETLK = 8,
    PU_F_SETLKW = 9,
    PU_F_DUPFD_CLOEXEC = 14,
};

/* Wire format for F_GETLK/F_SETLK/F_SETLKW's struct flock* argument (EDX) —
 * a plain, explicit ABI struct rather than newlib's own struct flock
 * (short/long fields), mirrored field-for-field by user/newlib_syscalls.c's
 * fcntl() before/after the syscall — same "kernel and newlib each keep
 * their own differently-shaped struct, translate at the boundary" pattern
 * as struct pureunix_stat / user/newlib_syscalls.c's struct pu_raw_stat.
 * l_whence is honored (SEEK_SET/CUR/END); l_pid is always 0 on a
 * successful F_GETLK conflict report — see include/pureunix/flock.h for
 * why a real pid isn't tracked. */
struct pu_raw_flock {
    int32_t l_type;
    int32_t l_whence;
    int32_t l_start;
    int32_t l_len;
    int32_t l_pid;
};

/* Every path-taking syscall below used to hand its raw path straight to
 * vfs_*() — fine as long as the only callers were the in-kernel shell
 * (which always pre-resolves against its own ctx->cwd via vfs_normalize()
 * before calling anything, see shell/sh.c) or test suites that only ever
 * used absolute paths. Once real, independent multi-process programs
 * (BusyBox's coreutils, ash) started making raw relative-path syscalls
 * directly — plain `stat(".")`, `open("foo")`, etc. — that gap became a
 * real bug (a relative path was always treated as relative to the
 * filesystem root, since nothing here consulted the calling task's own
 * cwd). This resolves every path against task_current_cwd() (SYS_CHDIR's
 * handler already did this correctly; this generalizes the same pattern to
 * every other syscall that takes a path). Safe for already-absolute paths
 * too — vfs_normalize() with an absolute `path` just canonicalizes it. */
static void resolve_path(char *out, const char *path)
{
    vfs_normalize(out, task_current_cwd(), path);
}

/* Collects vfs_readdir() callback entries into the caller's SYS_READDIR
 * output buffer, capped at the caller-supplied capacity. */
typedef struct readdir_collect_ctx {
    struct pureunix_dirent *out;
    int max;
    int count;
} readdir_collect_ctx_t;

static int readdir_collect_cb(const vfs_dirent_t *entry, void *ctx_)
{
    readdir_collect_ctx_t *ctx = ctx_;
    if (ctx->count >= ctx->max) {
        return 1; /* stop iteration: buffer full */
    }
    struct pureunix_dirent *d = &ctx->out[ctx->count];
    strncpy(d->name, entry->name, PUREUNIX_MAX_NAME - 1);
    d->name[PUREUNIX_MAX_NAME - 1] = '\0';
    d->type = (uint32_t)entry->type;
    d->size = entry->size;
    ctx->count++;
    return 0;
}

/* fds 0/1/2 all name the same single console tty (see drivers/tty.c) as
 * long as they still hold their default console binding (file == NULL —
 * see include/pureunix/task.h); dup2()ing something else onto one of them
 * makes it a real, non-tty fd, same as a real UNIX process redirecting
 * its own stdin/stdout/stderr. Anything else is either a bad descriptor or
 * a real open file that just isn't a terminal. Shared by SYS_TCGETATTR and
 * SYS_TCSETATTR. */
/* /dev/tty1..NUM_VTS name a specific VT directly (1-based, matching the
 * vtN numbers users see); /dev/tty is the calling task's own controlling
 * terminal (its task_t.vt_id). Returns the 0-based VT id SYS_OPEN should
 * bind the new descriptor to, or -1 if `path` isn't a console device path
 * at all. See include/pureunix/vt.h and SYS_OPEN's FD_KIND_TTY case below. */
static int dev_tty_path_vt(const char *path)
{
    if (strcmp(path, "/dev/tty") == 0) {
        task_t *t = task_current();
        int vt_id = t ? t->vt_id : -1;
        return vt_id >= 0 ? vt_id : 0;
    }
    if (strncmp(path, "/dev/tty", 8) != 0) {
        return -1;
    }
    const char *num = path + 8;
    if (num[0] < '1' || num[0] > '9' || num[1] != '\0') {
        return -1;
    }
    int n = num[0] - '0';
    if (n < 1 || n > NUM_VTS) {
        return -1;
    }
    return n - 1;
}

static int tty_fd_check(int fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES) {
        return -EBADF;
    }
    task_t *t = task_current();
    if (!t || !t->fds[fd].used) {
        return -EBADF;
    }
    open_file_t *f = t->fds[fd].file;
    if ((fd == 0 || fd == 1 || fd == 2) && !f) {
        return 0;
    }
    /* An explicit /dev/ttyN fd (SYS_OPEN's /dev/tty interception above) is
     * a real open_file_t, but a real tty all the same -- unlike a
     * FD_KIND_FILE/FD_KIND_PIPE fd, which never is. Same for either end of
     * a real PTY pair (include/pureunix/pty.h) -- a pty is a real tty too,
     * it just isn't one of the 6 physical VTs. */
    if (f && (f->kind == FD_KIND_TTY || f->kind == FD_KIND_PTY)) {
        return 0;
    }
    return f ? -ENOTTY : -EBADF;
}

/* Which VT `fd` (already tty_fd_check()-validated by the caller) names —
 * an explicit /dev/ttyN fd (FD_KIND_TTY) names the VT it was opened
 * against; the fd 0/1/2 default console binding names `caller`'s own
 * vt_id. Shared by VT_GETACTIVE/TIOCGPGRP/TIOCSPGRP (SYS_IOCTL below). */
static int fd_to_vt_id(task_t *caller, int fd)
{
    open_file_t *of = caller->fds[fd].file;
    return (of && of->kind == FD_KIND_TTY) ? of->tty_vt_id
                                            : (caller->vt_id >= 0 ? caller->vt_id : 0);
}

/* Finds the lowest fd >= start that's free for a *new* allocation
 * (open()/pipe()/dup()/fcntl(F_DUPFD)) — real POSIX "lowest available fd"
 * semantics. A slot is free if it's simply unused, OR it's fd 0/1/2
 * specifically that was *explicitly* close()'d back to their console-bound
 * state (used == true, file == NULL, closed_explicitly == true — see
 * include/pureunix/task.h's fd_entry_t comment for the full reasoning).
 * Two other cases also end up `used == true, file == NULL` on 0/1/2 and
 * must NOT be treated as free: never touched at all (the task-creation-time
 * default), and actively holding a deliberate console binding again after
 * a dup2()-based redirect-then-restore cycle (what every real shell,
 * including BusyBox ash, does around a redirected builtin) — `used` alone
 * can't tell any of the three apart, hence `closed_explicitly`.
 *
 * Real programs rely on the explicitly-closed case: BusyBox's `uniq FILE`
 * (and several other coreutils' optional-FILE-argument handling) does
 * `close(0); open(path)` specifically to make the opened file *become* fd
 * 0 — every allocator here used to start the search at index 3
 * specifically to dodge 0/1/2 altogether, which defeated that idiom and
 * left it blocked reading the console instead of the file it thought it
 * just opened.
 *
 * The exception is deliberately restricted to i < 3: for fd >= 3, close()
 * actually marks the slot `used = false` when it's freed (see SYS_CLOSE),
 * so `!used` alone is already the right, sufficient test there. Returns -1
 * if none is free. */
static int find_free_fd(task_t *t, int start)
{
    if (start < 0) {
        start = 0;
    }
    for (int i = start; i < MAX_OPEN_FILES; i++) {
        bool console_reclaimable = i < 3 && !t->fds[i].file && t->fds[i].closed_explicitly;
        if (!t->fds[i].used || console_reclaimable) {
            return i;
        }
    }
    return -1;
}

/* ---- Pipes (SYS_PIPE/SYS_DUP/SYS_DUP2) --------------------------------
 * A fixed-size ring buffer (pipe_buf_t, include/pureunix/task.h) shared by
 * both ends' open_file_t. Blocked reads/writes sleep on the pipe_buf_t's
 * own read_wq/write_wq (kernel/wait.c) rather than spinning — see
 * include/pureunix/wait.h. This only actually accomplishes anything if
 * some other task (typically a forked child on the other end of the
 * pipe) is what wakes it by moving data — a single task written to read
 * its own pipe with nothing else able to run would block forever, exactly
 * like a real UNIX pipe in that same situation. */

typedef struct {
    pipe_buf_t *p;
} pipe_wait_ctx_t;

static bool pipe_readable(void *ctx)
{
    pipe_buf_t *p = ((pipe_wait_ctx_t *)ctx)->p;
    return p->count > 0 || p->write_ends == 0;
}

static bool pipe_writable(void *ctx)
{
    pipe_buf_t *p = ((pipe_wait_ctx_t *)ctx)->p;
    return p->count < PUREUNIX_PIPE_SIZE || p->read_ends == 0;
}

static int pipe_read(open_file_t *f, char *buf, size_t len)
{
    if (f->pipe_is_write_end) {
        return -EBADF;
    }
    pipe_buf_t *p = f->pipe_buf;
    if (len == 0) {
        return 0;
    }
    if (p->count == 0) {
        if (p->write_ends == 0) {
            return 0; /* EOF: no writers left, nothing buffered */
        }
        /* See include/pureunix/wait.h's invariant — int $0x80 enters with
         * interrupts masked, and nothing could ever wake this sleeper
         * otherwise. */
        arch_enable_interrupts();
        pipe_wait_ctx_t ctx = { .p = p };
        wait_queue_sleep(&p->read_wq, pipe_readable, &ctx);
        if (p->count == 0) {
            return 0; /* woken because the last writer closed, not by data */
        }
    }
    size_t to_copy = len < p->count ? len : p->count;
    for (size_t i = 0; i < to_copy; ++i) {
        buf[i] = (char)p->data[p->tail];
        p->tail = (p->tail + 1) % PUREUNIX_PIPE_SIZE;
    }
    p->count -= to_copy;
    /* Freed up ring-buffer space — wake any writer blocked on a full pipe. */
    wait_queue_wake_all(&p->write_wq);
    return (int)to_copy;
}

static int pipe_write(open_file_t *f, const char *buf, size_t len)
{
    if (!f->pipe_is_write_end) {
        return -EBADF;
    }
    pipe_buf_t *p = f->pipe_buf;
    if (len == 0) {
        return 0;
    }
    if (p->read_ends == 0) {
        return -EPIPE;
    }
    size_t written = 0;
    while (written < len) {
        if (p->count == PUREUNIX_PIPE_SIZE) {
            arch_enable_interrupts();
            pipe_wait_ctx_t ctx = { .p = p };
            wait_queue_sleep(&p->write_wq, pipe_writable, &ctx);
            if (p->read_ends == 0) {
                return written ? (int)written : -EPIPE;
            }
        }
        size_t space = PUREUNIX_PIPE_SIZE - p->count;
        size_t chunk = (len - written) < space ? (len - written) : space;
        for (size_t i = 0; i < chunk; ++i) {
            p->data[p->head] = (uint8_t)buf[written + i];
            p->head = (p->head + 1) % PUREUNIX_PIPE_SIZE;
        }
        p->count += chunk;
        written += chunk;
        /* New data available — wake any reader blocked on an empty pipe. */
        wait_queue_wake_all(&p->read_wq);
    }
    return (int)written;
}

/* SYS_OPEN's error paths, after open_file_alloc() but before the new
 * open_file_t is ever installed into a fd table slot: releases it back to
 * the pool without the "flush to the VFS" side effect open_file_unref()
 * (a real close()) performs — nothing was ever really opened, so nothing
 * should be written. Frees f->data in case it was already populated
 * (currently only reachable in the read-only path's vfs_read_file()
 * failure case, where it's actually always NULL, but safe regardless). */
static void open_file_discard(open_file_t *f)
{
    kfree(f->data);
    f->refcount = 0;
}

void syscall_init(void)
{
}

uint32_t syscall_dispatch(interrupt_regs_t *regs)
{
    switch (regs->eax) {
    case SYS_EXIT:
        /* Deliberately a pass-through, not a real exit(): see
         * docs/syscalls.md and user/systest.c's "SYS_EXIT does not
         * terminate the caller" test. Actual process termination happens
         * through a separate, non-syscall mechanism — see int
         * $0x81 / task_terminate_trap in crt0.S and arch/i386/idt.c. */
        return regs->ebx;
    case SYS_WRITE: {
        int fd = (int)regs->ebx;
        const char *buf = (const char *)regs->ecx;
        size_t len = regs->edx;

        if (fd < 0 || fd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }
        task_t *t = task_current();
        if (!t || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        open_file_t *f = t->fds[fd].file;

        if ((fd == 1 || fd == 2) && !f) {
            /* Default console binding — see include/pureunix/task.h. Routed
             * through the writing task's *own* VT (vt_putc()/vt_write()),
             * not the physically active one: a backgrounded VT's process
             * (e.g. `ping` left running on VT2 while VT1 is on screen)
             * must update only its own saved console buffer, never bleed
             * output onto whatever VT the user actually has on screen. */
            vt_write(t->vt_id, buf, len);
            return len;
        }
        if (!f) {
            return (uint32_t)-EBADF;
        }
        if (!buf) {
            return (uint32_t)-EINVAL;
        }

        if (f->kind == FD_KIND_PIPE) {
            return (uint32_t)pipe_write(f, buf, len);
        }
        if (f->kind == FD_KIND_PTY) {
            int r = f->pty_is_master ? pty_master_write(f->pty, buf, len)
                                       : pty_slave_write(f->pty, buf, len);
            return (uint32_t)r;
        }
        if (f->kind == FD_KIND_NULL) {
            /* /dev/null: every byte is accepted and discarded — see
             * include/pureunix/task.h's FD_KIND_NULL comment. */
            return (uint32_t)len;
        }
        if (f->kind == FD_KIND_TTY) {
            /* /dev/ttyN opened explicitly -- see SYS_OPEN's /dev/tty
             * interception below and SYS_READ's matching FD_KIND_TTY
             * branch above. */
            vt_write(f->tty_vt_id, buf, len);
            return (uint32_t)len;
        }

        /* FD_KIND_FILE: writes accumulate in a kmalloc'd in-memory buffer
         * (growing it as needed) and are flushed to the underlying
         * filesystem in one shot on close() — see open_file_unref()
         * (kernel/task.c). */
        if (!(f->flags & O_WRONLY)) {
            return (uint32_t)-EBADF;
        }
        if (len == 0) {
            return 0;
        }

        size_t new_end = f->offset + len;
        if (new_end > f->size) {
            uint8_t *grown = kmalloc(new_end);
            if (!grown) {
                return (uint32_t)-ENOSPC;
            }
            memset(grown, 0, new_end);
            if (f->data) {
                memcpy(grown, f->data, f->size);
            }
            kfree(f->data);
            f->data = grown;
            f->size = new_end;
        }
        memcpy(f->data + f->offset, buf, len);
        f->offset += len;
        return (uint32_t)len;
    }
    case SYS_READ: {
        int fd = (int)regs->ebx;
        char *buf = (char *)regs->ecx;
        size_t len = (size_t)regs->edx;

        if (fd < 0 || fd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }
        task_t *t = task_current();
        if (!t || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        open_file_t *f = t->fds[fd].file;

        if (fd == 0 && !f) {
            /* stdin: routed through the termios-aware console tty driver —
             * see drivers/tty.c. Behaves like the classic canonical/echoing
             * console unless SYS_TCSETATTR has put it in raw mode. Targets
             * the calling task's own VT (t->vt_id; -1 falls back to VT1,
             * same as drivers/keyboard.c/drivers/tty.c's own fallback). */
            return (uint32_t)tty_read(t->vt_id >= 0 ? t->vt_id : 0, buf, len);
        }
        if (!f) {
            return (uint32_t)-EBADF;
        }
        if (!buf) {
            return (uint32_t)-EINVAL;
        }

        if (f->kind == FD_KIND_PIPE) {
            return (uint32_t)pipe_read(f, buf, len);
        }
        if (f->kind == FD_KIND_PTY) {
            int r = f->pty_is_master ? pty_master_read(f->pty, buf, len)
                                       : pty_slave_read(f->pty, buf, len);
            return (uint32_t)r;
        }
        if (f->kind == FD_KIND_NULL) {
            /* /dev/null: always reports EOF — see
             * include/pureunix/task.h's FD_KIND_NULL comment. */
            return 0;
        }
        if (f->kind == FD_KIND_TTY) {
            /* /dev/ttyN opened explicitly (SYS_OPEN's /dev/tty interception
             * below) -- blocks against that specific VT's own input queue,
             * regardless of which VT the caller itself belongs to (e.g.
             * reading /dev/tty3 from a shell on VT1 blocks until VT3 is
             * actually active, exactly like a real Linux tty device node). */
            return (uint32_t)tty_read(f->tty_vt_id, buf, len);
        }

        /* Zero-length read: valid per POSIX; nothing to copy */
        if (len == 0) {
            return 0;
        }

        /* At or past EOF */
        if (f->offset >= f->size) {
            return 0;
        }

        size_t available = f->size - f->offset;
        size_t to_copy   = (len < available) ? len : available;

        memcpy(buf, f->data + f->offset, to_copy);
        f->offset += to_copy;

        return (uint32_t)to_copy;
    }
    case SYS_GETPID:
        return task_current() ? task_current()->id : 0;
    case SYS_YIELD:
        task_yield();
        return 0;
    case SYS_OPEN: {
        const char *raw_path = (const char *)regs->ebx;
        int         flags = (int)regs->ecx;

        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        const char *path = path_buf;

        if (strcmp(path, "/dev/null") == 0) {
            task_t *t = task_current();
            int fd = find_free_fd(t, 0);
            if (fd < 0) {
                return (uint32_t)-EMFILE;
            }
            open_file_t *f = open_file_alloc(FD_KIND_NULL);
            if (!f) {
                return (uint32_t)-ENOSPC;
            }
            f->flags = flags;
            t->fds[fd].used = true;
            t->fds[fd].closed_explicitly = false;
            t->fds[fd].cloexec = false;
            t->fds[fd].file = f;
            return (uint32_t)fd;
        }

        if (strcmp(path, "/dev/tty") == 0) {
            task_t *ct = task_current();
            if (ct && ct->ctty_pty) {
                /* This task's controlling terminal is a real PTY
                 * (include/pureunix/pty.h), not one of the 6 physical VTs
                 * -- e.g. a shell PUTerm spawned (docs/pude.md). Must be
                 * checked before dev_tty_path_vt() below, which otherwise
                 * unconditionally treats "/dev/tty" as naming a VT (BusyBox
                 * ash's setjobctl() opens exactly this path for its own
                 * job-control ioctls -- without this, it would silently
                 * get a VT-bound fd instead of its real controlling
                 * terminal). */
                int fd = find_free_fd(ct, 0);
                if (fd < 0) {
                    return (uint32_t)-EMFILE;
                }
                open_file_t *f = open_file_alloc(FD_KIND_PTY);
                if (!f) {
                    return (uint32_t)-ENOSPC;
                }
                f->flags = flags;
                f->pty = ct->ctty_pty;
                f->pty_is_master = false;
                pty_slave_ref(f->pty);
                ct->fds[fd].used = true;
                ct->fds[fd].closed_explicitly = false;
                ct->fds[fd].cloexec = false;
                ct->fds[fd].file = f;
                return (uint32_t)fd;
            }
        }

        int dev_vt = dev_tty_path_vt(path);
        if (dev_vt >= 0) {
            /* /dev/ttyN / /dev/tty: a real fd bound to a specific VT (see
             * include/pureunix/vt.h), intercepted before any of the normal
             * VFS-backed open() logic below -- there is no on-disk content
             * to read, just kernel/vt.c's own console/keyboard-queue state. */
            task_t *t = task_current();
            int fd = find_free_fd(t, 0);
            if (fd < 0) {
                return (uint32_t)-EMFILE;
            }
            open_file_t *f = open_file_alloc(FD_KIND_TTY);
            if (!f) {
                return (uint32_t)-ENOSPC;
            }
            f->flags = flags;
            f->tty_vt_id = dev_vt;
            t->fds[fd].used = true;
            t->fds[fd].closed_explicitly = false;
            t->fds[fd].cloexec = false;
            t->fds[fd].file = f;
            return (uint32_t)fd;
        }

        bool want_write  = (flags & O_WRONLY) != 0;
        bool want_creat  = (flags & O_CREAT) != 0;
        bool want_append = (flags & O_APPEND) != 0;
        bool want_trunc  = (flags & O_TRUNC) != 0;

        task_t *t = task_current();
        int fd = find_free_fd(t, 0);
        if (fd < 0) {
            return (uint32_t)-EMFILE;
        }

        open_file_t *f = open_file_alloc(FD_KIND_FILE);
        if (!f) {
            return (uint32_t)-ENOSPC;
        }
        f->flags = flags;
        strncpy(f->path, path, PUREUNIX_MAX_PATH - 1);
        f->path[PUREUNIX_MAX_PATH - 1] = '\0';

        if (!want_write) {
            /* Read-only open (unchanged since Stage 3A). */
            vfs_stat_t st;
            int srs = vfs_stat(path, &st);
            if (srs != 0) {
                open_file_discard(f);
                return (uint32_t)srs;
            }
            if (st.type != VFS_FILE) {
                open_file_discard(f);
                return (uint32_t)-EISDIR;
            }
            if (!vfs_access(&st, current_uid(), current_gid(), R_OK)) {
                open_file_discard(f);
                return (uint32_t)-EACCES;
            }

            int rrc = vfs_read_file(path, &f->data, &f->size);
            if (rrc != 0) {
                open_file_discard(f);
                return (uint32_t)rrc;
            }

            t->fds[fd].used = true;
            t->fds[fd].closed_explicitly = false;
            t->fds[fd].cloexec = false;
            t->fds[fd].file = f;
            return (uint32_t)fd;
        }

        /* Writable open (Stage 4): the whole file is buffered in memory and
         * flushed to the filesystem in one shot on close() — see
         * open_file_unref() (kernel/task.c). */
        vfs_stat_t st;
        int sws = vfs_stat(path, &st);
        if (sws != 0) {
            /* Anything other than "doesn't exist" (e.g. -ELOOP, -EACCES
             * from a traversal check) is a real resolution failure and
             * must be reported as such, not papered over as ENOENT. */
            if (sws != -ENOENT) {
                open_file_discard(f);
                return (uint32_t)sws;
            }
            if (!want_creat) {
                open_file_discard(f);
                return (uint32_t)-ENOENT;
            }
            int cr = vfs_create(path);
            if (cr != 0 && cr != -EEXIST) {
                open_file_discard(f);
                return (uint32_t)cr;
            }
            sws = vfs_stat(path, &st);
            if (sws != 0) {
                open_file_discard(f);
                return (uint32_t)sws;
            }
        }
        if (st.type != VFS_FILE) {
            open_file_discard(f);
            return (uint32_t)-EISDIR;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), W_OK)) {
            open_file_discard(f);
            return (uint32_t)-EACCES;
        }

        /* Preserve existing content unless the caller explicitly asked to
         * discard it (O_TRUNC) — real POSIX open() semantics: O_WRONLY/
         * O_RDWR alone must never silently discard a file's data. Used to
         * only do this for O_APPEND, so any writable, non-append,
         * non-truncating open of an EXISTING file started from an empty
         * in-memory buffer and clobbered real content with zero bytes at
         * close() — found via the SQLite port (docs/sqlite-port.md):
         * SQLite always reopens its db file O_RDWR|O_CREAT with no
         * O_TRUNC, the first thing in this project to actually depend on
         * a writable reopen seeing prior content instead of always
         * meaning "start over" (every previous writer — ash's `>`
         * redirection, editors' save paths, coreutils — always pairs a
         * writable open with O_TRUNC when it wants a blank slate, so this
         * fix changes nothing for any of them). */
        if (!want_trunc) {
            vfs_read_file(path, &f->data, &f->size); /* best-effort; empty is fine (new file) */
        }
        f->offset = want_append ? f->size : 0;

        t->fds[fd].used = true;
        t->fds[fd].closed_explicitly = false;
        t->fds[fd].cloexec = false;
        t->fds[fd].file = f;
        return (uint32_t)fd;
    }
    case SYS_CLOSE: {
        int fd = (int)regs->ebx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        int rc = open_file_unref(t->fds[fd].file);
        t->fds[fd].file = NULL;
        if (fd >= 3) {
            t->fds[fd].used = false;
        } else {
            /* fd 0/1/2: left `used = true`, `file = NULL` — reverts to the
             * default console binding rather than becoming truly closed
             * (see include/pureunix/task.h's fd_entry_t comment). Setting
             * `closed_explicitly` here is what makes this state
             * distinguishable from the other two ways a slot ends up
             * `file == NULL` (never touched; or a dup2()-based
             * redirect-then-restore cycle put a console binding back
             * deliberately) — only *this*, a real close(), is fair game
             * for the next open()/dup()/pipe()/fcntl(F_DUPFD) to reclaim. */
            t->fds[fd].closed_explicitly = true;
        }
        return (uint32_t)rc;
    }
    case SYS_LSEEK: {
        int fd     = (int)regs->ebx;
        int offset = (int)regs->ecx;
        int whence = (int)regs->edx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file) {
            return (uint32_t)-EBADF;
        }
        open_file_t *f = t->fds[fd].file;
        if (f->kind != FD_KIND_FILE) {
            return (uint32_t)-EINVAL; /* pipes aren't seekable */
        }
        int new_offset;
        switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (int)f->offset + offset;
            break;
        case SEEK_END:
            new_offset = (int)f->size + offset;
            break;
        default:
            return (uint32_t)-EINVAL;
        }
        if (new_offset < 0) {
            return (uint32_t)-EINVAL;
        }
        f->offset = (size_t)new_offset;
        return (uint32_t)new_offset;
    }
    case SYS_PIPE: {
        int *fds_out = (int *)regs->ebx;
        if (!fds_out) {
            return (uint32_t)-EINVAL;
        }
        task_t *t = task_current();
        int read_fd = -1, write_fd = -1;
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            bool console_reclaimable = i < 3 && !t->fds[i].file && t->fds[i].closed_explicitly;
            if (!t->fds[i].used || console_reclaimable) {
                if (read_fd < 0) {
                    read_fd = i;
                } else {
                    write_fd = i;
                    break;
                }
            }
        }
        if (read_fd < 0 || write_fd < 0) {
            return (uint32_t)-EMFILE;
        }

        pipe_buf_t *p = kcalloc(1, sizeof(pipe_buf_t));
        open_file_t *rf = open_file_alloc(FD_KIND_PIPE);
        open_file_t *wf = open_file_alloc(FD_KIND_PIPE);
        if (!p || !rf || !wf) {
            kfree(p);
            kfree(rf);
            kfree(wf);
            return (uint32_t)-ENOSPC;
        }
        p->read_ends = 1;
        p->write_ends = 1;
        rf->pipe_buf = p;
        rf->pipe_is_write_end = false;
        wf->pipe_buf = p;
        wf->pipe_is_write_end = true;
        wf->flags = O_WRONLY;

        t->fds[read_fd].used = true;
        t->fds[read_fd].closed_explicitly = false;
        t->fds[read_fd].cloexec = false;
        t->fds[read_fd].file = rf;
        t->fds[write_fd].used = true;
        t->fds[write_fd].closed_explicitly = false;
        t->fds[write_fd].cloexec = false;
        t->fds[write_fd].file = wf;

        fds_out[0] = read_fd;
        fds_out[1] = write_fd;
        return 0;
    }
    case SYS_DUP: {
        int oldfd = (int)regs->ebx;
        task_t *t = task_current();
        if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !t->fds[oldfd].used) {
            return (uint32_t)-EBADF;
        }
        int newfd = find_free_fd(t, 0);
        if (newfd < 0) {
            return (uint32_t)-EMFILE;
        }
        t->fds[newfd].used = true;
        t->fds[newfd].closed_explicitly = false;
        t->fds[newfd].file = t->fds[oldfd].file;
        t->fds[newfd].cloexec = false;
        open_file_ref(t->fds[newfd].file);
        return (uint32_t)newfd;
    }
    case SYS_DUP2: {
        int oldfd = (int)regs->ebx;
        int newfd = (int)regs->ecx;
        task_t *t = task_current();
        if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !t->fds[oldfd].used) {
            return (uint32_t)-EBADF;
        }
        if (newfd < 0 || newfd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }
        if (oldfd == newfd) {
            return (uint32_t)newfd;
        }
        if (t->fds[newfd].used) {
            open_file_unref(t->fds[newfd].file);
        }
        t->fds[newfd].used = true;
        t->fds[newfd].closed_explicitly = false;
        t->fds[newfd].file = t->fds[oldfd].file;
        t->fds[newfd].cloexec = false;
        open_file_ref(t->fds[newfd].file);
        return (uint32_t)newfd;
    }
    case SYS_KILL: {
        int pid = (int)regs->ebx;
        int sig = (int)regs->ecx;
        task_t *t = task_current();

        if (pid == 0) {
            /* kill(0, sig): every process in the caller's own group. */
            if (sig != 0) {
                signal_send_pgrp(t->pgid, sig);
                task_yield_if_not_running(t);
            }
            return 0;
        }
        if (pid < 0) {
            /* kill(-pgid, sig): an explicit process group. */
            uint32_t pgid = (uint32_t)(-pid);
            if (!task_pgrp_exists(pgid)) {
                return (uint32_t)-ESRCH;
            }
            if (sig != 0) {
                signal_send_pgrp(pgid, sig);
                task_yield_if_not_running(t);
            }
            return 0;
        }

        /* pid > 0: a single target. */
        if (t && (uint32_t)pid == t->id) {
            if (sig == 0) {
                return 0; /* the null signal: self always "exists" */
            }
            /* Real delivery (default action / SIG_IGN / a real handler —
             * kernel/signal.c). If the default action terminates or
             * stops this same task, task_yield_if_not_running() makes
             * sure it stops running *right now* rather than falling
             * through to this syscall's normal iret-back-to-ring3 return
             * path on a stale assumption that it's still runnable. */
            signal_send(t, sig);
            task_yield_if_not_running(t);
            return 0;
        }
        if (task_kill((uint32_t)pid, sig) != 0) {
            return (uint32_t)-ESRCH;
        }
        return 0;
    }
    case SYS_SIGACTION: {
        int sig = (int)regs->ebx;
        const pu_sigaction_t *act = (const pu_sigaction_t *)regs->ecx;
        pu_sigaction_t *old = (pu_sigaction_t *)regs->edx;
        task_t *t = task_current();
        if (sig <= 0 || sig >= 32) {
            return (uint32_t)-EINVAL;
        }
        if (sig == SIGKILL || sig == SIGSTOP) {
            /* POSIX: disposition of these two can never be changed. */
            return (uint32_t)-EINVAL;
        }
        if (old) {
            old->handler = t->sig_handlers[sig];
        }
        if (act) {
            t->sig_handlers[sig] = act->handler;
        }
        return 0;
    }
    case SYS_SIGPROCMASK: {
        int how = (int)regs->ebx;
        const uint32_t *set = (const uint32_t *)regs->ecx;
        uint32_t *old = (uint32_t *)regs->edx;
        task_t *t = task_current();
        if (old) {
            *old = t->blocked_signals;
        }
        if (set) {
            uint32_t s = *set;
            /* SIGKILL/SIGSTOP can never be blocked — POSIX. */
            s &= ~((1u << SIGKILL) | (1u << SIGSTOP));
            switch (how) {
            case PU_SIG_BLOCK:   t->blocked_signals |= s; break;
            case PU_SIG_UNBLOCK: t->blocked_signals &= ~s; break;
            case PU_SIG_SETMASK: t->blocked_signals = s; break;
            default: return (uint32_t)-EINVAL;
            }
        }
        return 0;
    }
    case SYS_SIGPENDING: {
        uint32_t *out = (uint32_t *)regs->ebx;
        if (!out) {
            return (uint32_t)-EINVAL;
        }
        *out = task_current()->pending_signals;
        return 0;
    }
    case SYS_SETPRIORITY: {
        uint32_t pid = (uint32_t)regs->ebx;
        int nice = (int)regs->ecx;
        return (uint32_t)task_setpriority(pid, nice);
    }
    case SYS_GETPRIORITY: {
        uint32_t pid = (uint32_t)regs->ebx;
        int *out = (int *)regs->ecx;
        int nice = 0;
        int rc = task_getpriority(pid, &nice);
        if (rc != 0) {
            return (uint32_t)rc;
        }
        if (out) {
            *out = nice;
        }
        return 0;
    }
    case SYS_FCNTL: {
        /* Minimal fcntl(): only the operations BusyBox's dd/ash actually
         * need. F_GETFD/F_SETFD are still honest no-ops (nothing sets the
         * close-on-exec bit through them — see fd_entry_t.cloexec, only
         * ever set by F_DUPFD_CLOEXEC below and honored by kernel/elf.c's
         * exec() paths); a real fcntl(fd, F_SETFD, FD_CLOEXEC) caller isn't
         * exercised by anything in this userland today. */
        int fd  = (int)regs->ebx;
        int cmd = (int)regs->ecx;
        int arg = (int)regs->edx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        switch (cmd) {
        case PU_F_GETFD:
            return 0;
        case PU_F_SETFD:
            return 0;
        case PU_F_GETFL:
            return t->fds[fd].file ? (uint32_t)t->fds[fd].file->flags : 0;
        case PU_F_SETFL:
            if (t->fds[fd].file) {
                t->fds[fd].file->flags = arg;
            }
            return 0;
        case PU_F_GETLK:
        case PU_F_SETLK:
        case PU_F_SETLKW: {
            /* F_SETLKW is handled identically to F_SETLK (non-blocking) —
             * see include/pureunix/flock.h: SQLite's default build (no
             * SQLITE_ENABLE_SETLK_TIMEOUT) never actually issues an
             * F_SETLKW, so there is deliberately no blocking/wait-queue
             * path here to get wrong. */
            struct pu_raw_flock *lk = (struct pu_raw_flock *)(uintptr_t)arg;
            open_file_t *f = t->fds[fd].file;
            if (!lk || !f || f->kind != FD_KIND_FILE) {
                return (uint32_t)-EINVAL;
            }
            int32_t start = lk->l_start;
            if (lk->l_whence == SEEK_CUR) {
                start += (int32_t)f->offset;
            } else if (lk->l_whence == SEEK_END) {
                start += (int32_t)f->size;
            }
            if (cmd == PU_F_GETLK) {
                int16_t type = (int16_t)lk->l_type;
                int32_t s = start, l = lk->l_len;
                flock_getlk(f->path, f, &type, &s, &l);
                lk->l_type = type;
                if (type != PU_F_UNLCK) {
                    lk->l_whence = SEEK_SET;
                    lk->l_start = s;
                    lk->l_len = l;
                    lk->l_pid = 0;
                }
                return 0;
            }
            return (uint32_t)flock_setlk(f->path, f, (int16_t)lk->l_type, start, lk->l_len);
        }
        case PU_F_DUPFD:
        case PU_F_DUPFD_CLOEXEC: {
            /* F_DUPFD_CLOEXEC's close-on-exec bit (fd_entry_t.cloexec) is
             * real, honored by kernel/elf.c's exec() paths — see that
             * field's own comment. F_DUPFD leaves it false, matching
             * POSIX (a plain dup never sets close-on-exec).
             *
             * ash's savefd() — called on every fd it's about to redirect,
             * including a plain interactive shell's own still-untouched
             * stdout the first time it ever runs `cmd > file` —
             * unconditionally uses F_DUPFD_CLOEXEC (newlib defines it as
             * 14, a distinct value from F_DUPFD's 0, not an alias), and
             * treats any fcntl() failure other than EBADF as fatal to the
             * whole interpreter. Not recognizing this command at all used
             * to fall through to -EINVAL below, which savefd() raises as
             * fatal — killing the shell outright on its first redirection.
             *
             * Also, like SYS_DUP/SYS_DUP2, this must duplicate a NULL file
             * (fd 0/1/2 start out "used" but console-bound, file == NULL —
             * see include/pureunix/task.h's fd_entry_t comment) rather than
             * reject it — open_file_ref(NULL) is already a safe no-op. */
            int newfd = find_free_fd(t, arg < 0 ? 0 : arg);
            if (newfd < 0) {
                return (uint32_t)-EMFILE;
            }
            t->fds[newfd].used = true;
            t->fds[newfd].closed_explicitly = false;
            t->fds[newfd].file = t->fds[fd].file;
            t->fds[newfd].cloexec = (cmd == PU_F_DUPFD_CLOEXEC);
            open_file_ref(t->fds[newfd].file);
            return (uint32_t)newfd;
        }
        default:
            return (uint32_t)-EINVAL;
        }
    }
    case SYS_FTRUNCATE: {
        int fd = (int)regs->ebx;
        int length = (int)regs->ecx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file) {
            return (uint32_t)-EBADF;
        }
        if (length < 0) {
            return (uint32_t)-EINVAL;
        }
        open_file_t *f = t->fds[fd].file;
        if (f->kind != FD_KIND_FILE) {
            return (uint32_t)-EINVAL;
        }
        size_t new_size = (size_t)length;
        uint8_t *new_data = kmalloc(new_size ? new_size : 1);
        if (!new_data) {
            return (uint32_t)-ENOSPC;
        }
        size_t keep = f->size < new_size ? f->size : new_size;
        if (f->data && keep) {
            memcpy(new_data, f->data, keep);
        }
        if (new_size > keep) {
            memset(new_data + keep, 0, new_size - keep);
        }
        if (f->data) {
            kfree(f->data);
        }
        f->data = new_data;
        f->size = new_size;
        return 0;
    }
    case SYS_GETPPID: {
        task_t *t = task_current();
        return t ? t->ppid : 0;
    }
    case SYS_SETPGID:
        return (uint32_t)task_setpgid((uint32_t)regs->ebx, (uint32_t)regs->ecx);
    case SYS_GETPGID:
        return (uint32_t)task_getpgid((uint32_t)regs->ebx);
    case SYS_SETSID:
        return (uint32_t)task_setsid();
    case SYS_GETSID:
        return (uint32_t)task_getsid((uint32_t)regs->ebx);
    case SYS_STAT: {
        const char *raw_path = (const char *)regs->ebx;
        struct pureunix_stat *st = (struct pureunix_stat *)regs->ecx;
        if (!raw_path || !st) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        vfs_stat_t vst;
        int src = vfs_stat(path_buf, &vst);
        if (src != 0) {
            return (uint32_t)src;
        }
        st->st_size = vst.size;
        st->st_type = (uint32_t)vst.type;
        st->st_attr = vst.mode;
        st->st_mode = vst.st_mode;
        st->st_uid = vst.st_uid;
        st->st_gid = vst.st_gid;
        st->st_nlink = vst.st_nlink;
        st->st_ino = vst.st_ino;
        st->st_atime = vst.st_atime;
        st->st_mtime = vst.st_mtime;
        st->st_ctime = vst.st_ctime;
        st->st_blocks = vst.st_blocks;
        st->st_blksize = vst.st_blksize;
        return 0;
    }
    case SYS_FSTAT: {
        int fd = (int)regs->ebx;
        struct pureunix_stat *st = (struct pureunix_stat *)regs->ecx;
        if (!st) {
            return (uint32_t)-EINVAL;
        }
        if (fd < 0 || fd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }
        task_t *t = task_current();
        if (!t || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        open_file_t *f = t->fds[fd].file;
        memset(st, 0, sizeof(*st));

        if ((fd == 0 || fd == 1 || fd == 2) && !f) {
            /* Default console binding, not a real open_file_t (see
             * include/pureunix/task.h's fd_entry_t comment) — report it
             * as the character device it is. */
            st->st_type = 1;
            st->st_mode = S_IFCHR | 0666;
            st->st_nlink = 1;
            st->st_blksize = 1024;
            return 0;
        }
        if (!f) {
            return (uint32_t)-EBADF;
        }
        if (f->kind == FD_KIND_PIPE) {
            st->st_type = 1;
            st->st_mode = S_IFIFO | 0600;
            st->st_nlink = 1;
            st->st_blksize = 4096;
            return 0;
        }
        if (f->kind == FD_KIND_NULL) {
            st->st_type = 1;
            st->st_mode = S_IFCHR | 0666;
            st->st_nlink = 1;
            st->st_blksize = 1024;
            return 0;
        }

        /* FD_KIND_FILE: re-stat the underlying path for full Unix metadata
         * (uid/gid/nlink/ino/timestamps/blocks), then override st_size
         * with the open file description's *live* in-memory size — for a
         * writable fd, f->size (not yet flushed to the VFS) is the
         * correct current size; for a read-only fd it already matches
         * on-disk size. This is what user/newlib_syscalls.c's fstat()
         * was missing entirely (it fabricated st_size=0 unconditionally,
         * with no kernel query at all — see docs/tcc-port.md-adjacent
         * bug writeup / commit message for the "sh: 3: m" investigation
         * this fixed: BusyBox ash sizes its script read buffer from
         * fstat(), so a bogus 0 made it treat every script as empty). */
        vfs_stat_t vst;
        int src = vfs_stat(f->path, &vst);
        if (src == 0) {
            st->st_type = (uint32_t)vst.type;
            st->st_attr = vst.mode;
            st->st_mode = vst.st_mode;
            st->st_uid = vst.st_uid;
            st->st_gid = vst.st_gid;
            st->st_nlink = vst.st_nlink;
            st->st_ino = vst.st_ino;
            st->st_atime = vst.st_atime;
            st->st_mtime = vst.st_mtime;
            st->st_ctime = vst.st_ctime;
            st->st_blocks = vst.st_blocks;
            st->st_blksize = vst.st_blksize;
        } else {
            /* Path no longer resolvable (e.g. unlinked while open) — still
             * report a plausible regular-file mode so callers relying on
             * S_ISREG()/st_size (the actual live data) keep working. */
            st->st_type = 1;
            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
            st->st_blksize = 1024;
        }
        st->st_size = (uint32_t)f->size;
        return 0;
    }
    case SYS_ACCESS: {
        const char *raw_path = (const char *)regs->ebx;
        int mode = (int)regs->ecx;

        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        vfs_stat_t st;
        int sra = vfs_stat(path_buf, &st);
        if (sra != 0) {
            return (uint32_t)sra;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), mode)) {
            return (uint32_t)-EACCES;
        }
        return 0;
    }
    case SYS_LSTAT: {
        const char *raw_path = (const char *)regs->ebx;
        struct pureunix_stat *st = (struct pureunix_stat *)regs->ecx;
        if (!raw_path || !st) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        vfs_stat_t vst;
        int rc = vfs_lstat(path_buf, &vst);
        if (rc != 0) {
            return (uint32_t)rc;
        }
        st->st_size = vst.size;
        st->st_type = (uint32_t)vst.type;
        st->st_attr = vst.mode;
        st->st_mode = vst.st_mode;
        st->st_uid = vst.st_uid;
        st->st_gid = vst.st_gid;
        st->st_nlink = vst.st_nlink;
        st->st_ino = vst.st_ino;
        st->st_atime = vst.st_atime;
        st->st_mtime = vst.st_mtime;
        st->st_ctime = vst.st_ctime;
        st->st_blocks = vst.st_blocks;
        st->st_blksize = vst.st_blksize;
        return 0;
    }
    case SYS_READLINK: {
        const char *raw_path = (const char *)regs->ebx;
        char *buf = (char *)regs->ecx;
        size_t bufsize = (size_t)regs->edx;
        if (!raw_path || !buf) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_readlink(path_buf, buf, bufsize);
    }
    case SYS_MKDIR: {
        const char *raw_path = (const char *)regs->ebx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_mkdir(path_buf);
    }
    case SYS_UNLINK: {
        const char *raw_path = (const char *)regs->ebx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_unlink(path_buf);
    }
    case SYS_RMDIR: {
        const char *raw_path = (const char *)regs->ebx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_rmdir(path_buf);
    }
    case SYS_RENAME: {
        const char *raw_old = (const char *)regs->ebx;
        const char *raw_new = (const char *)regs->ecx;
        if (!raw_old || !raw_new) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char old_buf[PUREUNIX_MAX_PATH], new_buf[PUREUNIX_MAX_PATH];
        resolve_path(old_buf, raw_old);
        resolve_path(new_buf, raw_new);
        return (uint32_t)vfs_rename(old_buf, new_buf);
    }
    case SYS_LINK: {
        const char *raw_old = (const char *)regs->ebx;
        const char *raw_new = (const char *)regs->ecx;
        if (!raw_old || !raw_new) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char old_buf[PUREUNIX_MAX_PATH], new_buf[PUREUNIX_MAX_PATH];
        resolve_path(old_buf, raw_old);
        resolve_path(new_buf, raw_new);
        return (uint32_t)vfs_link(old_buf, new_buf);
    }
    case SYS_SYMLINK: {
        /* target is a symlink's stored contents, an arbitrary string the
         * kernel never resolves itself (only the second argument, where
         * the symlink itself is created, is a real path) — left
         * unresolved on purpose, matching real symlink(2) semantics. */
        const char *target = (const char *)regs->ebx;
        const char *raw_path = (const char *)regs->ecx;
        if (!target || !raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_symlink(target, path_buf);
    }
    case SYS_CHMOD: {
        const char *raw_path = (const char *)regs->ebx;
        mode_t mode = (mode_t)regs->ecx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_chmod(path_buf, mode);
    }
    case SYS_CHOWN: {
        const char *raw_path = (const char *)regs->ebx;
        uid_t uid = (uid_t)regs->ecx;
        gid_t gid = (gid_t)regs->edx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_chown(path_buf, uid, gid);
    }
    case SYS_FCHMOD: {
        int fd = (int)regs->ebx;
        mode_t mode = (mode_t)regs->ecx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file ||
            t->fds[fd].file->kind != FD_KIND_FILE) {
            return (uint32_t)-EBADF;
        }
        return (uint32_t)vfs_chmod(t->fds[fd].file->path, mode);
    }
    case SYS_FCHOWN: {
        int fd = (int)regs->ebx;
        uid_t uid = (uid_t)regs->ecx;
        gid_t gid = (gid_t)regs->edx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file ||
            t->fds[fd].file->kind != FD_KIND_FILE) {
            return (uint32_t)-EBADF;
        }
        return (uint32_t)vfs_chown(t->fds[fd].file->path, uid, gid);
    }
    case SYS_FUTIME: {
        int fd = (int)regs->ebx;
        uint32_t atime = regs->ecx;
        uint32_t mtime = regs->edx;
        task_t *t = task_current();
        if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file ||
            t->fds[fd].file->kind != FD_KIND_FILE) {
            return (uint32_t)-EBADF;
        }
        return (uint32_t)vfs_utime(t->fds[fd].file->path, atime, mtime);
    }
    case SYS_READDIR: {
        const char *raw_path = (const char *)regs->ebx;
        struct pureunix_dirent *out = (struct pureunix_dirent *)regs->ecx;
        int max = (int)regs->edx;

        if (!raw_path || !out || max <= 0) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        readdir_collect_ctx_t ctx = { .out = out, .max = max, .count = 0 };
        int drc = vfs_readdir(path_buf, readdir_collect_cb, &ctx);
        if (drc < 0) {
            return (uint32_t)drc;
        }
        return (uint32_t)ctx.count;
    }
    case SYS_FORK: {
        task_t *child = task_fork(regs);
        return child ? child->id : (uint32_t)-1;
    }
    case SYS_EXEC: {
        const char *path = (const char *)regs->ebx;
        /* argv/envp are user-space pointers, read while this task's own
         * address space is still the active one (elf_exec_current() only
         * switches CR3 after copying everything out of them — see its
         * comment). A NULL argv (ecx == 0, e.g. old callers/pu_exec())
         * means "just the bare path"; a NULL envp (edx == 0) means an
         * empty environment, same as real execve(path, argv, NULL). */
        char *const default_argv[] = { (char *)path, NULL };
        char *const *argv = (char *const *)regs->ecx;
        char *const *envp = (char *const *)regs->edx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        if (!argv) {
            argv = default_argv;
        }
        int argc = 0;
        while (argc < ELF_MAX_ARGS && argv[argc]) {
            argc++;
        }
        return (uint32_t)elf_exec_current(regs, path, argc, argv, envp);
    }
    case SYS_WAIT: {
        int pid = (int)regs->ebx;
        int *status = (int *)regs->ecx;
        int options = (int)regs->edx;
        int st = 0;
        /* task_waitpid() genuinely blocks now (task_t.child_wait) — see
         * include/pureunix/wait.h's invariant, same as pipe_read() above:
         * int $0x80 enters with interrupts masked, and nothing could ever
         * wake this sleeper otherwise. */
        arch_enable_interrupts();
        int rc = task_waitpid(pid, &st, options);
        if (rc >= 0 && status) {
            *status = st;
        }
        return (uint32_t)rc;
    }
    case SYS_TCGETATTR: {
        int fd = (int)regs->ebx;
        struct termios *out = (struct termios *)regs->ecx;
        int chk = tty_fd_check(fd);
        if (chk != 0) {
            return (uint32_t)chk;
        }
        open_file_t *f = task_current()->fds[fd].file;
        if (f && f->kind == FD_KIND_PTY) {
            return (uint32_t)pty_get_termios(f->pty, out);
        }
        return (uint32_t)tty_get_termios(out);
    }
    case SYS_TCSETATTR: {
        int fd = (int)regs->ebx;
        const struct termios *in = (const struct termios *)regs->ecx;
        int actions = (int)regs->edx;
        int chk = tty_fd_check(fd);
        if (chk != 0) {
            return (uint32_t)chk;
        }
        if (actions != TCSANOW && actions != TCSADRAIN && actions != TCSAFLUSH) {
            return (uint32_t)-EINVAL;
        }
        open_file_t *f = task_current()->fds[fd].file;
        if (f && f->kind == FD_KIND_PTY) {
            return (uint32_t)pty_set_termios(f->pty, in);
        }
        return (uint32_t)tty_set_termios(in);
    }
    case SYS_IOCTL: {
        int fd = (int)regs->ebx;
        int request = (int)regs->ecx;
        void *argp = (void *)regs->edx;
        int chk = tty_fd_check(fd);
        if (chk != 0) {
            return (uint32_t)chk;
        }

        open_file_t *pty_f = task_current()->fds[fd].file;
        if (pty_f && pty_f->kind == FD_KIND_PTY) {
            /* A real PTY fd (include/pureunix/pty.h) -- handled entirely
             * separately from the VT-based requests below, since
             * fd_to_vt_id() has no meaning for it. */
            pty_t *pt = pty_f->pty;
            if (request == TIOCGWINSZ) {
                if (!argp) {
                    return (uint32_t)-EINVAL;
                }
                struct winsize *ws = (struct winsize *)argp;
                unsigned short rows, cols;
                pty_get_winsize(pt, &rows, &cols);
                ws->ws_row = rows;
                ws->ws_col = cols;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                return 0;
            }
            if (request == TIOCSWINSZ) {
                if (!argp) {
                    return (uint32_t)-EINVAL;
                }
                const struct winsize *ws = (const struct winsize *)argp;
                pty_set_winsize(pt, ws->ws_row, ws->ws_col);
                /* Real pty semantics: setting the window size signals the
                 * foreground job so a termios-aware program (ash's line
                 * editor, vi, ncurses apps) actually notices and re-queries
                 * TIOCGWINSZ, exactly matching kernel/vt.c's own
                 * vt_signal_resize() for TIOCSFONT on a physical VT.
                 * Harmless no-op if nothing has claimed this pty's
                 * foreground group yet (pgid 0 never matches any real
                 * task). */
                int fg_pgid = pty_get_fg_pgid(pt);
                if (fg_pgid > 0) {
                    signal_send_pgrp((uint32_t)fg_pgid, SIGWINCH);
                }
                return 0;
            }
            if (request == TIOCGPGRP) {
                if (!argp) {
                    return (uint32_t)-EINVAL;
                }
                int pgid = pty_get_fg_pgid(pt);
                if (pgid == 0) {
                    return (uint32_t)-ENOTTY;
                }
                *(int *)argp = pgid;
                return 0;
            }
            if (request == TIOCSPGRP) {
                if (!argp) {
                    return (uint32_t)-EINVAL;
                }
                task_t *caller = task_current();
                if (pty_get_fg_pgid(pt) == 0 || caller->sid == 0) {
                    return (uint32_t)-ENOTTY;
                }
                int pgid = *(int *)argp;
                if (pgid <= 0) {
                    return (uint32_t)-EINVAL;
                }
                task_t *member = task_find((uint32_t)pgid);
                bool same_session_group = member && member->sid == caller->sid &&
                                           (uint32_t)pgid == member->pgid;
                if (!same_session_group) {
                    return (uint32_t)-EPERM;
                }
                pty_set_fg_pgid(pt, pgid);
                return 0;
            }
            if (request == TIOCSCTTY) {
                /* Real POSIX rule: only a session leader may acquire a
                 * controlling terminal. task_t.ctty_pty starts NULL for
                 * every task, so this is the only way it's ever set. */
                task_t *caller = task_current();
                if (caller->sid != caller->id) {
                    return (uint32_t)-EPERM;
                }
                caller->ctty_pty = pt;
                if (pty_get_sid(pt) == 0) {
                    pty_set_sid(pt, (int)caller->sid);
                    /* Real POSIX/kernel/vt.c's vt_claim_session() behavior:
                     * acquiring a fresh controlling terminal makes the
                     * acquiring process's own group the foreground one --
                     * without this, pty_get_fg_pgid() stays 0 forever
                     * (pty_alloc()'s kcalloc default), which is
                     * indistinguishable from "no controlling terminal" to
                     * TIOCGPGRP/TIOCSPGRP. A real caller (BusyBox ash's
                     * setjobctl()) calling tcgetpgrp() right after this
                     * ioctl would otherwise always get -ENOTTY and
                     * conclude "can't access tty; job control turned
                     * off" -- confirmed as the actual, reproducible cause
                     * of that exact message the first time PUTerm ran a
                     * real shell under this pty. */
                    pty_set_fg_pgid(pt, (int)caller->id);
                }
                return 0;
            }
            return (uint32_t)-EINVAL;
        }

        if (request == TIOCGWINSZ) {
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            struct winsize *ws = (struct winsize *)argp;
            size_t rows, cols;
            vga_get_size(&rows, &cols);
            ws->ws_row = (unsigned short)rows;
            ws->ws_col = (unsigned short)cols;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
        if (request == TIOCSFONT) {
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            int scale = *(int *)argp;
            if (!vga_apply_font_scale(scale)) {
                return (uint32_t)-EINVAL;
            }
            vt_signal_resize();
            return 0;
        }
        if (request == VT_GETACTIVE) {
            /* Which VT *this fd* names -- POSIX ttyname() semantics (the
             * `tty` command, user/tty.c, is a thin wrapper over exactly
             * this), not "whichever VT happens to be on screen right now"
             * (that's a separate, genuinely global question a background
             * VT's own `tty` should NOT answer with someone else's VT
             * number). An explicit /dev/ttyN fd (FD_KIND_TTY) reports the
             * VT it was opened against; the fd 0/1/2 default console
             * binding reports the calling task's own vt_id. */
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            *(int *)argp = fd_to_vt_id(task_current(), fd) + 1;
            return 0;
        }
        if (request == VT_ACTIVATE) {
            /* The exact same kernel VT-switching path Alt+F<n> uses (see
             * drivers/keyboard.c/hid.c) — the `tty N` command (user/tty.c)
             * reaches here via ioctl(fd, VT_ACTIVATE, &n) instead of
             * duplicating any switching logic of its own. */
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            int n = *(int *)argp;
            if (n < 1 || n > NUM_VTS) {
                return (uint32_t)-EINVAL;
            }
            vt_switch(n - 1);
            return 0;
        }
        if (request == TIOCGPGRP) {
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            int vt_id = fd_to_vt_id(task_current(), fd);
            int pgid = vt_get_fg_pgid(vt_id);
            if (pgid == 0) {
                return (uint32_t)-ENOTTY;
            }
            *(int *)argp = pgid;
            return 0;
        }
        if (request == TIOCSPGRP) {
            /* tcsetpgrp() — how a job-control-aware shell (BusyBox ash)
             * moves a job into the foreground of its controlling
             * terminal. Simplified versus full POSIX: requires the
             * caller's session to be the one that actually owns this VT
             * (so a background VT's session can't hijack another VT's
             * foreground group), and the target pgid to belong to some
             * live task in that same session — but does not otherwise
             * validate process-group *membership* beyond that (see
             * docs/process-management.md's scope notes). */
            if (!argp) {
                return (uint32_t)-EINVAL;
            }
            int vt_id = fd_to_vt_id(task_current(), fd);
            task_t *caller = task_current();
            if (vt_get_fg_pgid(vt_id) == 0 || (uint32_t)caller->sid == 0) {
                return (uint32_t)-ENOTTY;
            }
            int pgid = *(int *)argp;
            if (pgid <= 0) {
                return (uint32_t)-EINVAL;
            }
            task_t *member = task_find((uint32_t)pgid);
            bool same_session_group = member && member->sid == caller->sid &&
                                       (uint32_t)pgid == member->pgid;
            if (!same_session_group) {
                return (uint32_t)-EPERM;
            }
            vt_set_fg_pgid(vt_id, pgid);
            return 0;
        }
        return (uint32_t)-EINVAL;
    }
    case SYS_CHDIR: {
        const char *path = (const char *)regs->ebx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char resolved[PUREUNIX_MAX_PATH];
        vfs_normalize(resolved, task_current_cwd(), path);
        vfs_stat_t st;
        int rc = vfs_stat(resolved, &st);
        if (rc != 0) {
            return (uint32_t)rc;
        }
        if (!S_ISDIR(st.st_mode)) {
            return (uint32_t)-ENOTDIR;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), X_OK)) {
            return (uint32_t)-EACCES;
        }
        if (task_set_cwd(resolved) != 0) {
            return (uint32_t)-ENAMETOOLONG;
        }
        return 0;
    }
    case SYS_GETCWD: {
        char *buf = (char *)regs->ebx;
        size_t size = (size_t)regs->ecx;
        if (!buf || size == 0) {
            return (uint32_t)-EINVAL;
        }
        const char *cwd = task_current_cwd();
        size_t len = strlen(cwd);
        if (len + 1 > size) {
            return (uint32_t)-ERANGE;
        }
        memcpy(buf, cwd, len + 1);
        return 0;
    }
    case SYS_NANOSLEEP: {
        const struct pureunix_timespec *req = (const struct pureunix_timespec *)regs->ebx;
        struct pureunix_timespec *rem = (struct pureunix_timespec *)regs->ecx;
        if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
            return (uint32_t)-EINVAL;
        }
        uint32_t ms = (uint32_t)req->tv_sec * 1000u + (uint32_t)(req->tv_nsec / 1000000L);
        /* int $0x80 enters with interrupts masked (isr128, an interrupt
         * gate) and only re-enables them on its own iret -- pit_sleep()
         * needs the PIT tick interrupt to actually fire (and, now that it
         * also task_yield()s, needs other tasks' own interrupt-driven waits
         * to keep making progress too), so without this it hangs exactly
         * like SYS_PING's own pit_sleep() call already documents below. A
         * process sleeping between iterations (e.g. `ping`'s loop) is
         * exactly the case that must not freeze every other VT while it
         * waits — see docs/scheduler.md. */
        arch_enable_interrupts();
        pit_sleep(ms);
        /* No signal delivery exists yet (docs/syscalls.md's "Unimplemented
         * Syscalls"), so a sleep can never be interrupted early — it always
         * completes in full, leaving no remaining time to report. */
        if (rem) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return 0;
    }
    case SYS_GETUID:
        return (uint32_t)current_uid();
    case SYS_GETGID:
        return (uint32_t)current_gid();
    case SYS_UTIME: {
        const char *raw_path = (const char *)regs->ebx;
        uint32_t atime = regs->ecx;
        uint32_t mtime = regs->edx;
        if (!raw_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_utime(path_buf, atime, mtime);
    }
    case SYS_STATFS: {
        const char *raw_path = (const char *)regs->ebx;
        vfs_statfs_t *out = (vfs_statfs_t *)regs->ecx;
        if (!raw_path || !out) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        char path_buf[PUREUNIX_MAX_PATH];
        resolve_path(path_buf, raw_path);
        return (uint32_t)vfs_statfs(path_buf, out);
    }
    case SYS_SET_TLS: {
        uint32_t tp = regs->ebx;
        task_t *t = task_current();
        t->tls_base = tp;
        /* Takes effect immediately (not just on the next context switch) —
         * the calling task is about to load %gs itself and start using it
         * right after this syscall returns. */
        gdt_set_tls_base(tp);
        return 0;
    }
    case SYS_POLL: {
        /* Wire layout — must match user/newlib_syscalls.c's
         * struct pu_raw_pollfd exactly (int32_t/int16_t/int16_t, no
         * padding since it's already naturally 8-byte-sized/4-byte
         * aligned). Bit values must match user/newlib_compat/poll.h's
         * POLLIN/POLLOUT/POLLERR/POLLNVAL exactly (the kernel has no
         * <poll.h> of its own to pull these from). */
        enum { POLLIN = 0x1, POLLOUT = 0x4, POLLERR = 0x8, POLLNVAL = 0x20 };
        struct raw_pollfd { int32_t fd; int16_t events; int16_t revents; };
        struct raw_pollfd *ufds = (struct raw_pollfd *)regs->ebx;
        int nfds = (int)regs->ecx;
        int timeout_ms = (int)regs->edx;
        if (!ufds && nfds > 0) {
            return (uint32_t)-EINVAL;
        }
        task_t *t = task_current();
        int elapsed_ms = 0;
        for (;;) {
            int ready = 0;
            for (int i = 0; i < nfds; i++) {
                int fd = ufds[i].fd;
                int16_t events = ufds[i].events;
                int16_t revents = 0;
                if (fd < 0 || fd >= MAX_OPEN_FILES || !t->fds[fd].used || !t->fds[fd].file) {
                    revents = POLLNVAL;
                } else {
                    open_file_t *f = t->fds[fd].file;
                    if (f->kind == FD_KIND_PIPE) {
                        pipe_buf_t *pb = f->pipe_buf;
                        if (!f->pipe_is_write_end && (events & POLLIN) &&
                            (pb->count > 0 || pb->write_ends == 0)) {
                            revents |= POLLIN;
                        }
                        if (f->pipe_is_write_end && (events & POLLOUT) &&
                            (pb->count < PUREUNIX_PIPE_SIZE || pb->read_ends == 0)) {
                            revents |= POLLOUT;
                        }
                        if (f->pipe_is_write_end && pb->read_ends == 0) {
                            revents |= POLLERR;
                        }
                    } else {
                        /* No real readiness tracking exists yet for any
                         * other fd kind (regular file/tty/pty/procfs) —
                         * same honest-for-what-it-is optimistic fallback
                         * poll()/select() always gave every fd before real
                         * pipe support existed (see user/newlib_syscalls.c's
                         * poll() comment). */
                        revents = events & (POLLIN | POLLOUT);
                    }
                }
                ufds[i].revents = revents;
                if (revents) {
                    ready++;
                }
            }
            if (ready > 0 || timeout_ms == 0) {
                return (uint32_t)ready;
            }
            if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
                return 0;
            }
            /* int $0x80 enters with interrupts masked — pit_sleep() needs
             * the PIT tick interrupt to ever fire, same fix SYS_PING's
             * icmp_ping() and drivers/tty.c's tty_read() already apply
             * before their own blocking waits (see those comments / the
             * interrupt-gate-blocking gotcha in project memory). Short
             * 10ms steps rather than one real wait_queue_sleep(): pipe
             * readiness can depend on activity happening in a *different*
             * task, which would need waking this one from that task's own
             * write()/close() — real, but a larger change than this port
             * currently needs (its one real caller, Qt's own event
             * dispatcher wakeup pipe, is polled by the very same
             * single-threaded process that would also have to write it —
             * see docs/qt-port.md). */
            arch_enable_interrupts();
            pit_sleep(10);
            elapsed_ms += 10;
        }
    }
    case SYS_GETTIMEOFDAY: {
        uint32_t *out = (uint32_t *)regs->ebx;
        if (!out) {
            return (uint32_t)-EINVAL;
        }
        *out = time_now();
        return 0;
    }
    case SYS_PING: {
        ip4_addr_t dst = (ip4_addr_t)regs->ebx;
        uint32_t timeout_ms = regs->ecx;
        uint32_t *rtt_out = (uint32_t *)regs->edx;
        static uint16_t ping_seq;
        uint32_t rtt = 0;
        uint16_t id = task_current() ? (uint16_t)task_current()->id : 0;
        /* int $0x80 enters with interrupts masked (isr128, same as any
         * interrupt gate) and only re-enables them on its own iret --
         * icmp_ping() blocks on pit_sleep(), which needs the PIT tick
         * interrupt to actually fire, so without this it hangs exactly
         * like the RX-interrupt-context deadlock documented in net/ip.c's
         * ip_send() comment. Same fix drivers/tty.c's tty_read() already
         * applies before its own keyboard_getkey()-based blocking wait. */
        arch_enable_interrupts();
        bool ok = icmp_ping(dst, id, ++ping_seq, NULL, 0, timeout_ms, &rtt);
        if (rtt_out) {
            *rtt_out = rtt;
        }
        return ok ? 0 : (uint32_t)-ETIMEDOUT;
    }
    case SYS_INPUT_POLL: {
        pu_input_event_t *out = (pu_input_event_t *)regs->ebx;
        task_t *t = task_current();
        if (!out || !t) {
            return (uint32_t)-EINVAL;
        }
        int vt_id = t->vt_id >= 0 ? t->vt_id : 0;
        return vt_raw_input_try_get(vt_id, out) ? 1u : 0u;
    }
    case SYS_FB_GETINFO: {
        struct pureunix_fb_info *out = (struct pureunix_fb_info *)regs->ebx;
        if (!out) {
            return (uint32_t)-EINVAL;
        }
        const fb_info_t *fb = fb_get_info();
        if (!fb->present) {
            return (uint32_t)-ENODEV;
        }
        out->width = fb->width;
        out->height = fb->height;
        out->bypp = fb->bpp / 8;
        out->red_pos = fb->red_pos;
        out->red_size = fb->red_size;
        out->green_pos = fb->green_pos;
        out->green_size = fb->green_size;
        out->blue_pos = fb->blue_pos;
        out->blue_size = fb->blue_size;
        return 0;
    }
    case SYS_FB_BLIT: {
        const uint8_t *buf = (const uint8_t *)regs->ebx;
        size_t len = (size_t)regs->ecx;
        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EINVAL;
        }
        int vt_id = t->vt_id >= 0 ? t->vt_id : 0;
        if (!vt_is_graphics_mode(vt_id) || vt_active_id() != vt_id) {
            /* Backgrounded or not-yet-graphics-mode: succeed with no
             * hardware effect, mirroring vt_write()'s existing
             * backgrounded-VT behavior — see include/pureunix/syscall.h's
             * SYS_FB_BLIT comment. */
            return 0;
        }
        return (uint32_t)fb_blit_buffer(buf, len);
    }
    case SYS_GET_TICKS_MS:
        return (uint32_t)(pit_ticks() * 10);
    case SYS_SET_GRAPHICS_MODE: {
        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EINVAL;
        }
        int vt_id = t->vt_id >= 0 ? t->vt_id : 0;
        vt_set_graphics_mode(vt_id, regs->ebx != 0);
        return 0;
    }
    case SYS_FB_MMAP: {
        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EINVAL;
        }
        if (t->fb_shadow_mapped) {
            return t->heap_base + HEAP_MAX;
        }
        const fb_info_t *fb = fb_get_info();
        if (!fb->present) {
            return (uint32_t)-ENODEV;
        }
        size_t needed = (size_t)fb->width * fb->height * (fb->bpp / 8);
        uint32_t pages = (uint32_t)((needed + PUREUNIX_PAGE_SIZE - 1) / PUREUNIX_PAGE_SIZE);
        uint32_t fb_shadow_va = t->heap_base + HEAP_MAX;
        for (uint32_t i = 0; i < pages; ++i) {
            phys_addr_t frame = pmm_alloc_frame();
            if (!frame) {
                /* Partial mapping left in place on failure -- matches this
                 * process's normal address-space teardown (task exit/
                 * fork-failure paths already free whatever's present in
                 * the user window unconditionally), and a program that
                 * gets -ENOMEM here has no reasonable way to continue
                 * anyway. */
                return (uint32_t)-ENOMEM;
            }
            memset((void *)(uintptr_t)frame, 0, PUREUNIX_PAGE_SIZE);
            vmm_map_page_in(t->pd_phys, fb_shadow_va + i * PUREUNIX_PAGE_SIZE, frame,
                             PAGE_USER | PAGE_WRITE);
        }
        t->fb_shadow_mapped = true;
        return fb_shadow_va;
    }
    case SYS_SBRK: {
        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EINVAL;
        }
        int32_t incr = (int32_t)regs->ebx;
        uint32_t old_break = t->heap_base + t->heap_used;
        if (incr < 0 && (uint32_t)(-incr) > t->heap_used) {
            return (uint32_t)-EINVAL;
        }
        uint32_t new_used = (uint32_t)((int32_t)t->heap_used + incr);
        if (new_used > HEAP_MAX) {
            return (uint32_t)-ENOMEM;
        }
        uint32_t new_needed = ALIGN_UP(new_used, PUREUNIX_PAGE_SIZE);
        while (t->heap_mapped < new_needed) {
            phys_addr_t frame = pmm_alloc_frame();
            if (!frame) {
                return (uint32_t)-ENOMEM;
            }
            memset((void *)(uintptr_t)frame, 0, PUREUNIX_PAGE_SIZE);
            vmm_map_page_in(t->pd_phys, t->heap_base + t->heap_mapped, frame, PAGE_USER | PAGE_WRITE);
            t->heap_mapped += PUREUNIX_PAGE_SIZE;
        }
        t->heap_used = new_used;
        return old_break;
    }
    case SYS_PTY_CREATE: {
        int *fds_out = (int *)regs->ebx;
        if (!fds_out) {
            return (uint32_t)-EINVAL;
        }
        task_t *t = task_current();
        int master_fd = -1, slave_fd = -1;
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            bool console_reclaimable = i < 3 && !t->fds[i].file && t->fds[i].closed_explicitly;
            if (!t->fds[i].used || console_reclaimable) {
                if (master_fd < 0) {
                    master_fd = i;
                } else {
                    slave_fd = i;
                    break;
                }
            }
        }
        if (master_fd < 0 || slave_fd < 0) {
            return (uint32_t)-EMFILE;
        }

        pty_t *pt = pty_alloc();
        if (!pt) {
            return (uint32_t)-ENOSPC;
        }
        open_file_t *mf = open_file_alloc(FD_KIND_PTY);
        open_file_t *sf = open_file_alloc(FD_KIND_PTY);
        if (!mf || !sf) {
            if (mf) {
                open_file_unref(mf);
            }
            if (sf) {
                open_file_unref(sf);
            }
            return (uint32_t)-ENOSPC;
        }
        mf->pty = pt;
        mf->pty_is_master = true;
        pty_master_ref(pt);
        sf->pty = pt;
        sf->pty_is_master = false;
        pty_slave_ref(pt);

        t->fds[master_fd].used = true;
        t->fds[master_fd].closed_explicitly = false;
        t->fds[master_fd].cloexec = false;
        t->fds[master_fd].file = mf;
        t->fds[slave_fd].used = true;
        t->fds[slave_fd].closed_explicitly = false;
        t->fds[slave_fd].cloexec = false;
        t->fds[slave_fd].file = sf;

        fds_out[0] = master_fd;
        fds_out[1] = slave_fd;
        return 0;
    }
    case SYS_DEBUG_SETCRED: {
        /* Test-only credential override — see the comment on
         * SYS_DEBUG_SETCRED in include/pureunix/syscall.h. No privilege
         * check by design: there is no login system yet to check against. */
        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EINVAL;
        }
        t->uid = (uid_t)regs->ebx;
        t->gid = (gid_t)regs->ecx;
        return 0;
    }
    default:
        return (uint32_t)-1;
    }
}
