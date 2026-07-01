#include <pureunix/arch.h>
#include <pureunix/dirent.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/keyboard.h>
#include <pureunix/memory.h>
#include <pureunix/stat.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>

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

void syscall_init(void)
{
}

uint32_t syscall_dispatch(interrupt_regs_t *regs)
{
    switch (regs->eax) {
    case SYS_EXIT:
        return regs->ebx;
    case SYS_WRITE: {
        int fd = (int)regs->ebx;
        const char *buf = (const char *)regs->ecx;
        size_t len = regs->edx;
        if (fd != 1 && fd != 2) {
            return (uint32_t)-EBADF;
        }
        for (size_t i = 0; i < len; ++i) {
            putchar(buf[i]);
        }
        return len;
    }
    case SYS_READ: {
        int fd = (int)regs->ebx;
        char *buf = (char *)regs->ecx;
        size_t len = (size_t)regs->edx;

        if (fd == 0) {
            /* stdin: keyboard — preserve original behaviour */
            if (!buf) {
                return (uint32_t)-EINVAL;
            }
            size_t i = 0;
            while (i < len) {
                int key = keyboard_getkey();
                if (key == KEY_ENTER) {
                    buf[i++] = '\n';
                    break;
                }
                if (key > 0 && key < 128) {
                    buf[i++] = (char)key;
                }
            }
            return (uint32_t)i;
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
        /* Only O_RDONLY is implemented. */
        if (flags != O_RDONLY) {
            return (uint32_t)-EINVAL;
        }
        if (!vfs_mounted()) {
            return (uint32_t)-ENOENT;
        }

        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {
            return (uint32_t)-ENOENT;
        }
        if (st.type != VFS_FILE) {
            return (uint32_t)-EISDIR;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), R_OK)) {
            return (uint32_t)-EACCES;
        }

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

        uint8_t *data = NULL;
        size_t   size = 0;
        if (vfs_read_file(path, &data, &size) != 0) {
            return (uint32_t)-ENOENT;
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
    case SYS_CLOSE: {
        int fd = (int)regs->ebx;
        task_t *t = task_current();
        if (fd < 3 || fd >= MAX_OPEN_FILES || !t->fds[fd].used) {
            return (uint32_t)-EBADF;
        }
        kfree(t->fds[fd].data);
        memset(&t->fds[fd], 0, sizeof(t->fds[fd]));
        return 0;
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
        if (vfs_stat(path, &vst) != 0) {
            return (uint32_t)-ENOENT;
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
        if (vfs_stat(path, &st) != 0) {
            return (uint32_t)-ENOENT;
        }
        if (!vfs_access(&st, current_uid(), current_gid(), mode)) {
            return (uint32_t)-EACCES;
        }
        return 0;
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
        if (vfs_readdir(path, readdir_collect_cb, &ctx) < 0) {
            return (uint32_t)-ENOENT;
        }
        return (uint32_t)ctx.count;
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
