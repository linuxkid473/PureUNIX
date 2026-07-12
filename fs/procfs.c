/* fs/procfs.c — a minimal, synthetic, read-only /proc. See
 * include/pureunix/procfs.h's header comment for why this exists at all
 * (BusyBox's ps/top applets hard-require it) and docs/procfs.md for the
 * exact file list.
 *
 * Implements the existing vfs_ops_t (fs/vfs.c) the same way FAT16's
 * read-only subset does: only stat/read_file/readdir are non-NULL, so
 * every mutating operation (write/create/unlink/chmod/...) automatically
 * fails with "unsupported" at the VFS layer — no procfs-specific
 * rejection logic needed. Every file's content is synthesized fresh into
 * a kmalloc'd buffer on each read_file() call (the same "materialize the
 * whole file at open() time" model this kernel already uses for real
 * files — see include/pureunix/task.h's open_file_t) — there is no
 * on-disk (or even persistent in-memory) backing store, it's generated
 * on demand from kernel/task.c's task_list()/kernel/pmm.c's own state.
 *
 * IMPORTANT: this kernel's printf()/snprintf() family (libc/printf.c)
 * does NOT actually support 64-bit arguments despite parsing an 'l'
 * length modifier — passing a uint64_t (e.g. task_t.cpu_ticks) directly
 * to a "%lu" would silently misalign every argument after it in the same
 * call. Every value formatted here is deliberately truncated to a
 * uint32_t first — safe for any realistic uptime on this kernel. */
#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/errno.h>
#include <pureunix/memory.h>
#include <pureunix/procfs.h>
#include <pureunix/stat.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vmm.h>

/* One-character process state, matching Linux's /proc/[pid]/stat and
 * `ps`'s own STAT column conventions closely enough for libbb/procps.c's
 * parsing (it treats this field opaquely, just displays it back). */
static char state_char(task_state_t s)
{
    switch (s) {
    case TASK_RUNNING:   return 'R';
    case TASK_RUNNABLE:  return 'R';
    case TASK_SLEEPING:  return 'S';
    case TASK_STOPPED:   return 'T';
    case TASK_ZOMBIE:    return 'Z';
    default:             return '?';
    }
}

/* Parses a leading "/<digits>" path component into a pid, and leaves
 * *out_rest pointing just past it (either "" for exactly "/<pid>", or
 * "/whatever" for "/<pid>/whatever"). Returns false if path doesn't
 * start with a decimal pid at all (the global /proc/stat, /proc/uptime,
 * /proc/meminfo files, or a malformed request). */
static bool parse_pid_path(const char *path, uint32_t *out_pid, const char **out_rest)
{
    if (!path || path[0] != '/' || path[1] < '0' || path[1] > '9') {
        return false;
    }
    uint32_t pid = 0;
    const char *p = path + 1;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *out_pid = pid;
    *out_rest = p;
    return true;
}

/* Real per-process memory footprint: task_t.mapped_bytes (kernel/elf.c's
 * elf_load_into()) is the genuine sum of every PT_LOAD segment's page-
 * aligned size plus the fixed stack region, computed at the process's
 * last exec() — not a fixed per-task constant, so this now actually
 * varies between e.g. a tiny "hello" and ncdemo's ~1.3 MiB of linked
 * ncurses. Still an approximation of Linux's vsize/rss (this kernel maps
 * every page of a process eagerly at exec() time rather than lazily on
 * first touch, so there's no separate "resident" subset smaller than
 * "mapped" the way a real demand-paged kernel would report) — but a
 * real, per-program-varying number rather than the same constant for
 * every process. 0 for a kernel-only task (never execs a user ELF). */
static uint32_t approx_vsize_bytes(const task_t *t)
{
    return t->mapped_bytes;
}

static int render_pid_stat(const task_t *t, char **out_data, size_t *out_size)
{
    char *buf = kmalloc(512);
    if (!buf) {
        return -ENOMEM;
    }
    /* Field order/count matches Linux's /proc/[pid]/stat through field
     * 24 (rss) — see docs/procfs.md for exactly which fields are real
     * vs. zero-filled placeholders. utime/stime are in this kernel's own
     * 100 Hz PIT tick units, which happens to equal Linux's traditional
     * USER_HZ=100, so no scaling is needed. vsize is bytes; rss is
     * *pages*, not bytes — a common real-world bug source, called out
     * explicitly here since this synthesizes both from the same
     * approx_vsize_bytes() helper. */
    uint32_t vsize = approx_vsize_bytes(t);
    uint32_t rss_pages = vsize / PUREUNIX_PAGE_SIZE;
    int n = snprintf(buf, 512,
        "%u (%s) %c %u %u %u 0 0 0 0 0 0 0 %u 0 0 0 %d %d 1 0 %u %u %u\n",
        t->id, t->name, state_char(t->state), t->ppid, t->pgid, t->sid,
        (uint32_t)(t->cpu_ticks & 0xFFFFFFFFu), t->nice, t->nice,
        t->start_time, vsize, rss_pages);
    if (n < 0) {
        kfree(buf);
        return -EIO;
    }
    *out_data = buf;
    *out_size = strlen(buf);
    return 0;
}

static int render_pid_cmdline(const task_t *t, char **out_data, size_t *out_size)
{
    /* t->cmdline is already NUL-separated argv (kernel/elf.c's
     * pack_cmdline()) — exactly Linux's /proc/[pid]/cmdline format, so
     * this just copies it verbatim, trailing NULs and all. */
    size_t len = sizeof(t->cmdline);
    /* Trim trailing NUL padding beyond the last real argument so the
     * reported file size matches its actual content, not the whole
     * fixed-size struct field. */
    while (len > 0 && t->cmdline[len - 1] == '\0') {
        len--;
    }
    /* ...but keep exactly one trailing NUL, matching a real
     * /proc/[pid]/cmdline's own trailing terminator. */
    len++;
    if (len > sizeof(t->cmdline)) {
        len = sizeof(t->cmdline);
    }
    char *buf = kmalloc(len);
    if (!buf) {
        return -ENOMEM;
    }
    memcpy(buf, t->cmdline, len);
    *out_data = buf;
    *out_size = len;
    return 0;
}

static int render_pid_status(const task_t *t, char **out_data, size_t *out_size)
{
    char *buf = kmalloc(256);
    if (!buf) {
        return -ENOMEM;
    }
    int n = snprintf(buf, 256,
        "Name:\t%s\n"
        "State:\t%c\n"
        "Pid:\t%u\n"
        "PPid:\t%u\n"
        "PGid:\t%u\n"
        "SId:\t%u\n"
        "Uid:\t%u\n"
        "Gid:\t%u\n"
        "VmRSS:\t%u kB\n",
        t->name, state_char(t->state), t->id, t->ppid, t->pgid, t->sid,
        (uint32_t)t->uid, (uint32_t)t->gid,
        approx_vsize_bytes(t) / 1024);
    if (n < 0) {
        kfree(buf);
        return -EIO;
    }
    *out_data = buf;
    *out_size = strlen(buf);
    return 0;
}

/* /proc/stat: only the one line top.c/procps.c actually needs (the
 * aggregate "cpu ..." jiffy counters, for %CPU-over-time calculations).
 * "used" is the sum of every live task's own cpu_ticks (a reasonable
 * proxy for "non-idle" on a single-CPU system); "idle" is whatever's
 * left of the wall-clock tick count. Both are approximations — see
 * docs/procfs.md. */
typedef struct {
    uint32_t used_ticks;
} sum_ctx_t;

static void sum_cpu_ticks(const task_t *t, void *ctx)
{
    sum_ctx_t *c = ctx;
    c->used_ticks += (uint32_t)(t->cpu_ticks & 0xFFFFFFFFu);
}

static int render_proc_stat(char **out_data, size_t *out_size)
{
    sum_ctx_t ctx = { .used_ticks = 0 };
    task_list(sum_cpu_ticks, &ctx);
    uint32_t total = (uint32_t)(pit_ticks() & 0xFFFFFFFFu);
    uint32_t idle = total > ctx.used_ticks ? total - ctx.used_ticks : 0;
    char *buf = kmalloc(128);
    if (!buf) {
        return -ENOMEM;
    }
    int n = snprintf(buf, 128, "cpu  %u 0 0 %u 0 0 0 0 0 0\n", ctx.used_ticks, idle);
    if (n < 0) {
        kfree(buf);
        return -EIO;
    }
    *out_data = buf;
    *out_size = strlen(buf);
    return 0;
}

static int render_proc_uptime(char **out_data, size_t *out_size)
{
    uint32_t ticks = (uint32_t)(pit_ticks() & 0xFFFFFFFFu);
    uint32_t secs = ticks / 100;
    uint32_t hundredths = ticks % 100;
    char *buf = kmalloc(64);
    if (!buf) {
        return -ENOMEM;
    }
    int n = snprintf(buf, 64, "%u.%02u 0.00\n", secs, hundredths);
    if (n < 0) {
        kfree(buf);
        return -EIO;
    }
    *out_data = buf;
    *out_size = strlen(buf);
    return 0;
}

static int render_proc_meminfo(char **out_data, size_t *out_size)
{
    char *buf = kmalloc(128);
    if (!buf) {
        return -ENOMEM;
    }
    int n = snprintf(buf, 128, "MemTotal:%12u kB\nMemFree:%13u kB\n",
                      pmm_total_memory_kb(), pmm_free_memory_kb());
    if (n < 0) {
        kfree(buf);
        return -EIO;
    }
    *out_data = buf;
    *out_size = strlen(buf);
    return 0;
}

static int render_file_by_path(const char *path, char **out_data, size_t *out_size);

static int procfs_stat(const char *path, vfs_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;

    if (strcmp(path, "/") == 0) {
        st->type = VFS_DIR;
        st->st_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
        return 0;
    }
    if (strcmp(path, "/stat") == 0 || strcmp(path, "/uptime") == 0 ||
        strcmp(path, "/meminfo") == 0) {
        st->type = VFS_FILE;
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        char *buf = NULL;
        size_t size = 0;
        if (render_file_by_path(path, &buf, &size) == 0) {
            st->size = size;
            kfree(buf);
        }
        return 0;
    }

    uint32_t pid;
    const char *rest;
    if (!parse_pid_path(path, &pid, &rest)) {
        return -ENOENT;
    }
    task_t *t = task_find(pid);
    if (!t) {
        return -ENOENT;
    }
    if (rest[0] == '\0') {
        st->type = VFS_DIR;
        st->st_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
        return 0;
    }
    if (strcmp(rest, "/stat") == 0 || strcmp(rest, "/cmdline") == 0 ||
        strcmp(rest, "/status") == 0) {
        st->type = VFS_FILE;
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        char *buf = NULL;
        size_t size = 0;
        if (render_file_by_path(path, &buf, &size) == 0) {
            st->size = size;
            kfree(buf);
        }
        return 0;
    }
    return -ENOENT;
}

static int render_file_by_path(const char *path, char **out_data, size_t *out_size)
{
    if (strcmp(path, "/stat") == 0) {
        return render_proc_stat(out_data, out_size);
    }
    if (strcmp(path, "/uptime") == 0) {
        return render_proc_uptime(out_data, out_size);
    }
    if (strcmp(path, "/meminfo") == 0) {
        return render_proc_meminfo(out_data, out_size);
    }

    uint32_t pid;
    const char *rest;
    if (!parse_pid_path(path, &pid, &rest)) {
        return -ENOENT;
    }
    task_t *t = task_find(pid);
    if (!t) {
        return -ENOENT;
    }
    if (strcmp(rest, "/stat") == 0) {
        return render_pid_stat(t, out_data, out_size);
    }
    if (strcmp(rest, "/cmdline") == 0) {
        return render_pid_cmdline(t, out_data, out_size);
    }
    if (strcmp(rest, "/status") == 0) {
        return render_pid_status(t, out_data, out_size);
    }
    return -ENOENT;
}

static int procfs_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    return render_file_by_path(path, (char **)out_data, out_size);
}

typedef struct {
    vfs_readdir_cb_t cb;
    void *ctx;
} readdir_ctx_t;

static void emit_pid_dirent(const task_t *t, void *ctx)
{
    readdir_ctx_t *rc = ctx;
    vfs_dirent_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.name, sizeof(d.name), "%u", t->id);
    d.type = VFS_DIR;
    d.size = 0;
    rc->cb(&d, rc->ctx);
}

static int procfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    if (strcmp(path, "/") == 0) {
        static const char *const globals[] = { "stat", "uptime", "meminfo" };
        for (size_t i = 0; i < sizeof(globals) / sizeof(globals[0]); ++i) {
            vfs_dirent_t d;
            memset(&d, 0, sizeof(d));
            strncpy(d.name, globals[i], sizeof(d.name) - 1);
            d.type = VFS_FILE;
            if (cb(&d, ctx) != 0) {
                return 0;
            }
        }
        readdir_ctx_t rc = { .cb = cb, .ctx = ctx };
        task_list(emit_pid_dirent, &rc);
        return 0;
    }

    uint32_t pid;
    const char *rest;
    if (!parse_pid_path(path, &pid, &rest) || rest[0] != '\0') {
        return -ENOENT;
    }
    if (!task_find(pid)) {
        return -ENOENT;
    }
    static const char *const files[] = { "stat", "cmdline", "status" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        vfs_dirent_t d;
        memset(&d, 0, sizeof(d));
        strncpy(d.name, files[i], sizeof(d.name) - 1);
        d.type = VFS_FILE;
        if (cb(&d, ctx) != 0) {
            break;
        }
    }
    return 0;
}

static const vfs_ops_t procfs_ops = {
    .stat = procfs_stat,
    .read_file = procfs_read_file,
    .readdir = procfs_readdir,
};

const vfs_ops_t *procfs_vfs_ops(void)
{
    return &procfs_ops;
}
