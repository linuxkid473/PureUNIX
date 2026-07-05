#include <pureunix/arch.h>
#include <pureunix/dirent.h>
#include <pureunix/elf.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/ioctl.h>
#include <pureunix/memory.h>
#include <pureunix/stat.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/tty.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

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

/* fds 0/1/2 all name the same single console tty (see drivers/tty.c);
 * anything else is either a bad descriptor or a real open file that just
 * isn't a terminal. Shared by SYS_TCGETATTR and SYS_TCSETATTR. */
static int tty_fd_check(int fd)
{
    if (fd == 0 || fd == 1 || fd == 2) {
        return 0;
    }
    if (fd < 0 || fd >= MAX_OPEN_FILES) {
        return -EBADF;
    }
    task_t *t = task_current();
    if (t && t->fds[fd].used) {
        return -ENOTTY;
    }
    return -EBADF;
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

        if (fd == 1 || fd == 2) {
            for (size_t i = 0; i < len; ++i) {
                putchar(buf[i]);
            }
            return len;
        }

        /* Stage 4: writable file descriptors. Writes accumulate in a
         * kmalloc'd in-memory buffer (growing it as needed) and are flushed
         * to the underlying filesystem in one shot on close() — the same
         * "whole file lives in memory" model the read side already uses,
         * just mirrored for writes. */
        if (fd < 3 || fd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }

        task_t *t = task_current();
        fd_entry_t *f = &t->fds[fd];
        if (!f->used || !(f->flags & O_WRONLY)) {
            return (uint32_t)-EBADF;
        }
        if (!buf) {
            return (uint32_t)-EINVAL;
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

        if (fd == 0) {
            /* stdin: routed through the termios-aware console tty driver —
             * see drivers/tty.c. Behaves like the classic canonical/echoing
             * console unless SYS_TCSETATTR has put it in raw mode. */
            return (uint32_t)tty_read(buf, len);
        }

        /* fds 1 and 2 are write-only; anything outside [3, MAX_OPEN_FILES) is invalid */
        if (fd < 3 || fd >= MAX_OPEN_FILES) {
            return (uint32_t)-EBADF;
        }

        task_t *t = task_current();
        if (!t) {
            return (uint32_t)-EBADF;
        }

        fd_entry_t *f = &t->fds[fd];
        if (!f->used || !f->data) {
            return (uint32_t)-EBADF;
        }

        if (!buf) {
            return (uint32_t)-EINVAL;
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
        const char *path  = (const char *)regs->ebx;
        int         flags = (int)regs->ecx;

        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }

        bool want_write  = (flags & O_WRONLY) != 0;
        bool want_creat  = (flags & O_CREAT) != 0;
        bool want_append = (flags & O_APPEND) != 0;

        task_t *t = task_current();
        int fd = -1;
        for (int i = 3; i < MAX_OPEN_FILES; i++) {
            if (!t->fds[i].used) {
                fd = i;
                break;
            }
        }
        if (fd < 0) {
            return (uint32_t)-EMFILE;
        }

        if (!want_write) {
            /* Read-only open (unchanged since Stage 3A). */
            vfs_stat_t st;
            int srs = vfs_stat(path, &st);
            if (srs != 0) {
                return (uint32_t)srs;
            }
            if (st.type != VFS_FILE) {
                return (uint32_t)-EISDIR;
            }
            if (!vfs_access(&st, current_uid(), current_gid(), R_OK)) {
                return (uint32_t)-EACCES;
            }

            uint8_t *data = NULL;
            size_t   size = 0;
            int rrc = vfs_read_file(path, &data, &size);
            if (rrc != 0) {
                return (uint32_t)rrc;
            }

            t->fds[fd].used   = true;
            t->fds[fd].flags  = flags;
            t->fds[fd].data   = data;
            t->fds[fd].size   = size;
            t->fds[fd].offset = 0;
            strncpy(t->fds[fd].path, path, PUREUNIX_MAX_PATH - 1);
            t->fds[fd].path[PUREUNIX_MAX_PATH - 1] = '\0';
            return (uint32_t)fd;
        }

        /* Writable open (Stage 4): the whole file is buffered in memory and
         * flushed to the filesystem in one shot on close(). */
        vfs_stat_t st;
        int sws = vfs_stat(path, &st);
        if (sws != 0) {
            /* Anything other than "doesn't exist" (e.g. -ELOOP, -EACCES
             * from a traversal check) is a real resolution failure and
             * must be reported as such, not papered over as ENOENT. */
            if (sws != -ENOENT) {
                return (uint32_t)sws;
            }
            if (!want_creat) {
                return (uint32_t)-ENOENT;
            }
            int cr = vfs_create(path);
            if (cr != 0 && cr != -EEXIST) {
                return (uint32_t)cr;
            }
            sws = vfs_stat(path, &st);
            if (sws != 0) {
                return (uint32_t)sws;
            }
        }
        if (st.type != VFS_FILE) {
            return (uint32_t)-EISDIR;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), W_OK)) {
            return (uint32_t)-EACCES;
        }

        uint8_t *data = NULL;
        size_t   size = 0;
        if (want_append) {
            vfs_read_file(path, &data, &size); /* best-effort; empty is fine */
        }

        t->fds[fd].used   = true;
        t->fds[fd].flags  = flags;
        t->fds[fd].data   = data;
        t->fds[fd].size   = size;
        t->fds[fd].offset = want_append ? size : 0;
        strncpy(t->fds[fd].path, path, PUREUNIX_MAX_PATH - 1);
        t->fds[fd].path[PUREUNIX_MAX_PATH - 1] = '\0';
        return (uint32_t)fd;
    }
    case SYS_CLOSE: {
        int fd = (int)regs->ebx;
        task_t *t = task_current();
        if (fd < 3 || fd >= MAX_OPEN_FILES || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        fd_entry_t *f = &t->fds[fd];
        int rc = 0;
        if (f->flags & O_WRONLY) {
            int wr = vfs_write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size, 0);
            if (wr != 0) rc = wr;
        }
        kfree(f->data);
        memset(f, 0, sizeof(*f));
        return (uint32_t)rc;
    }
    case SYS_LSEEK: {
        int fd     = (int)regs->ebx;
        int offset = (int)regs->ecx;
        int whence = (int)regs->edx;
        task_t *t = task_current();
        if (fd < 3 || fd >= MAX_OPEN_FILES || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        int new_offset;
        switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (int)t->fds[fd].offset + offset;
            break;
        case SEEK_END:
            new_offset = (int)t->fds[fd].size + offset;
            break;
        default:
            return (uint32_t)-EINVAL;
        }
        if (new_offset < 0) {
            return (uint32_t)-EINVAL;
        }
        t->fds[fd].offset = (size_t)new_offset;
        return (uint32_t)new_offset;
    }
    case SYS_STAT: {
        const char *path = (const char *)regs->ebx;
        struct pureunix_stat *st = (struct pureunix_stat *)regs->ecx;
        if (!path || !st) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        vfs_stat_t vst;
        int src = vfs_stat(path, &vst);
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
    case SYS_ACCESS: {
        const char *path = (const char *)regs->ebx;
        int mode = (int)regs->ecx;

        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        vfs_stat_t st;
        int sra = vfs_stat(path, &st);
        if (sra != 0) {
            return (uint32_t)sra;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), mode)) {
            return (uint32_t)-EACCES;
        }
        return 0;
    }
    case SYS_LSTAT: {
        const char *path = (const char *)regs->ebx;
        struct pureunix_stat *st = (struct pureunix_stat *)regs->ecx;
        if (!path || !st) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        vfs_stat_t vst;
        int rc = vfs_lstat(path, &vst);
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
        const char *path = (const char *)regs->ebx;
        char *buf = (char *)regs->ecx;
        size_t bufsize = (size_t)regs->edx;
        if (!path || !buf) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_readlink(path, buf, bufsize);
    }
    case SYS_MKDIR: {
        const char *path = (const char *)regs->ebx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_mkdir(path);
    }
    case SYS_UNLINK: {
        const char *path = (const char *)regs->ebx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_unlink(path);
    }
    case SYS_RMDIR: {
        const char *path = (const char *)regs->ebx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_rmdir(path);
    }
    case SYS_RENAME: {
        const char *old_path = (const char *)regs->ebx;
        const char *new_path = (const char *)regs->ecx;
        if (!old_path || !new_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_rename(old_path, new_path);
    }
    case SYS_LINK: {
        const char *old_path = (const char *)regs->ebx;
        const char *new_path = (const char *)regs->ecx;
        if (!old_path || !new_path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_link(old_path, new_path);
    }
    case SYS_SYMLINK: {
        const char *target = (const char *)regs->ebx;
        const char *path = (const char *)regs->ecx;
        if (!target || !path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_symlink(target, path);
    }
    case SYS_CHMOD: {
        const char *path = (const char *)regs->ebx;
        mode_t mode = (mode_t)regs->ecx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_chmod(path, mode);
    }
    case SYS_CHOWN: {
        const char *path = (const char *)regs->ebx;
        uid_t uid = (uid_t)regs->ecx;
        gid_t gid = (gid_t)regs->edx;
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)vfs_chown(path, uid, gid);
    }
    case SYS_READDIR: {
        const char *path = (const char *)regs->ebx;
        struct pureunix_dirent *out = (struct pureunix_dirent *)regs->ecx;
        int max = (int)regs->edx;

        if (!path || !out || max <= 0) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        readdir_collect_ctx_t ctx = { .out = out, .max = max, .count = 0 };
        int drc = vfs_readdir(path, readdir_collect_cb, &ctx);
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
        if (!path) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)elf_exec_current(regs, path);
    }
    case SYS_WAIT: {
        int pid = (int)regs->ebx;
        int *status = (int *)regs->ecx;
        int st = 0;
        int rc = task_waitpid(pid, &st);
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
        if (request != TIOCGWINSZ) {
            return (uint32_t)-EINVAL;
        }
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
