#include <pureunix/errno.h>
#include <pureunix/stat.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>

/*
 * VFS mount layer.
 *
 * This file knows nothing about EXT2, FAT16, or any other concrete
 * filesystem. Drivers register themselves via vfs_mount(), supplying a
 * vfs_ops_t table of function pointers; every vfs_* entry point below
 * resolves a path to its mount (longest-prefix match) and calls through
 * that table. Filesystem-specific code lives only in the drivers
 * themselves and in whatever calls vfs_mount() at boot.
 *
 * Stage 3A adds Unix permission enforcement at this same layer, for the
 * same reason: drivers hand back raw metadata (vfs_stat_t::st_mode/st_uid/
 * st_gid) and know nothing about who's asking or what "permission" means.
 * Every public entry point below resolves the calling task's credentials
 * via current_uid()/current_gid() and runs them through vfs_access() (the
 * one central permission engine) before touching the driver.
 */

#define VFS_MAX_MOUNTS 8

static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static size_t mount_count = 0;

static char last_error[96] = "no error";

static void set_error(const char *msg)
{
    strncpy(last_error, msg, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

int vfs_init(void)
{
    mount_count = 0;
    set_error("no filesystem mounted");
    return 0;
}

bool vfs_mounted(void)
{
    return mount_count > 0;
}

int vfs_mount(const char *mountpoint, vfs_fs_type_t type, const vfs_ops_t *ops, void *fs_private)
{
    if (!mountpoint || !ops) return -1;

    for (size_t i = 0; i < mount_count; ++i) {
        if (strcmp(mount_table[i].mountpoint, mountpoint) == 0) {
            mount_table[i].type = type;
            mount_table[i].ops = ops;
            mount_table[i].fs_private = fs_private;
            return 0;
        }
    }

    if (mount_count >= VFS_MAX_MOUNTS) return -1;

    vfs_mount_t *m = &mount_table[mount_count++];
    strncpy(m->mountpoint, mountpoint, sizeof(m->mountpoint) - 1);
    m->mountpoint[sizeof(m->mountpoint) - 1] = '\0';
    m->type = type;
    m->ops = ops;
    m->fs_private = fs_private;
    return 0;
}

/* True if path falls under mountpoint mnt (length mnt_len), matching only
 * at a path-component boundary — "/fat" must not match "/fatabc". The root
 * mount ("/", mnt_len == 1) matches every absolute path. */
static bool path_under_mount(const char *path, const char *mnt, size_t mnt_len)
{
    if (strncmp(path, mnt, mnt_len) != 0) return false;
    if (mnt_len <= 1) return true;
    char next = path[mnt_len];
    return next == '\0' || next == '/';
}

const vfs_mount_t *vfs_find_mount(const char *path)
{
    if (!path || path[0] != '/') return NULL;

    const vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < mount_count; ++i) {
        size_t len = strlen(mount_table[i].mountpoint);
        if (path_under_mount(path, mount_table[i].mountpoint, len) && len >= best_len) {
            best = &mount_table[i];
            best_len = len;
        }
    }
    return best;
}

int vfs_mount_root(void)
{
    if (vfs_find_mount("/")) {
        set_error("no error");
        return 0;
    }
    set_error("no filesystem mounted");
    return -1;
}

/*
 * Resolve path to its mount and the sub-path the driver underneath should
 * see: the mountpoint prefix is stripped, and the result is re-rooted to
 * "/" if stripping would otherwise leave an empty string (e.g. "/fat" and
 * "/fat/" both become "/" for the FAT16 driver).
 */
static const vfs_mount_t *vfs_dispatch(const char *path, char *sub, size_t sub_size)
{
    const vfs_mount_t *m = vfs_find_mount(path);
    if (!m) return NULL;

    size_t mnt_len = strlen(m->mountpoint);
    const char *rest = (mnt_len <= 1) ? path : path + mnt_len;
    if (rest[0] == '\0') rest = "/";

    strncpy(sub, rest, sub_size - 1);
    sub[sub_size - 1] = '\0';
    return m;
}

/* Raw stat: dispatch + driver call, no permission checking. Used internally
 * by the permission engine itself (to inspect ancestor directories and
 * targets) so that check never recurses into itself. */
static int vfs_stat_raw(const char *path, vfs_stat_t *st)
{
    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->stat || m->ops->stat(sub, st) != 0) return -1;
    return 0;
}

/* Require X_OK on every directory in path's prefix (everything up to but
 * not including the final component) — the same "search permission on all
 * containing directories" rule POSIX applies to every path-taking call.
 * uid 0 always passes (root bypass, see vfs_access). */
static int check_traversal(const char *path, uid_t uid, gid_t gid)
{
    if (!path || path[0] != '/') return -1;

    size_t len = strlen(path);
    for (size_t i = 1; i < len; ++i) {
        if (path[i] != '/') continue;

        char prefix[PUREUNIX_MAX_PATH];
        size_t n = i < sizeof(prefix) - 1 ? i : sizeof(prefix) - 1;
        memcpy(prefix, path, n);
        prefix[n] = '\0';

        vfs_stat_t st;
        if (vfs_stat_raw(prefix, &st) != 0) {
            set_error("no such directory");
            return -1;
        }
        if (!vfs_access(&st, uid, gid, X_OK)) {
            set_error("permission denied");
            return -1;
        }
    }
    return 0;
}

/* Split path into its parent directory (out); used by the write-side calls
 * below to check permission on the directory being modified rather than on
 * a target that may not exist yet (create/mkdir) or is being removed from
 * its directory (unlink/rmdir/rename). */
static void parent_of(const char *path, char *out, size_t out_size)
{
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        strcpy(out, "/");
        return;
    }
    *slash = '\0';
}

/* Require W_OK on path's parent directory. */
static int check_dir_write(const char *path, uid_t uid, gid_t gid)
{
    char parent[PUREUNIX_MAX_PATH];
    parent_of(path, parent, sizeof(parent));

    vfs_stat_t pst;
    if (vfs_stat_raw(parent, &pst) != 0) {
        set_error("no such directory");
        return -1;
    }
    if (!vfs_access(&pst, uid, gid, W_OK)) {
        set_error("permission denied");
        return -1;
    }
    return 0;
}

/* ---- Read dispatch ---- */

int vfs_stat(const char *path, vfs_stat_t *st)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;

    if (vfs_stat_raw(path, st) != 0) {
        set_error("stat failed");
        return -1;
    }
    return 0;
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;

    vfs_stat_t st;
    if (vfs_stat_raw(path, &st) != 0) {
        set_error("read failed");
        return -1;
    }
    if (!vfs_access(&st, uid, gid, R_OK)) {
        set_error("permission denied");
        return -1;
    }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->read_file || m->ops->read_file(sub, out_data, out_size) != 0) {
        set_error("read failed");
        return -1;
    }
    return 0;
}

int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;

    vfs_stat_t st;
    if (vfs_stat_raw(path, &st) != 0) {
        set_error("readdir failed");
        return -1;
    }
    if (!vfs_access(&st, uid, gid, R_OK)) {
        set_error("permission denied");
        return -1;
    }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->readdir || m->ops->readdir(sub, cb, ctx) != 0) {
        set_error("readdir failed");
        return -1;
    }
    return 0;
}

/* ---- Write dispatch ----
 * EXT2 is read-only (write_file/create/unlink/rename are all NULL in its
 * ops table) so these permission checks are presently only exercised via
 * FAT16, but they run unconditionally: a future writable filesystem gets
 * enforcement for free, with no change needed here. */

int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;

    vfs_stat_t st;
    if (vfs_stat_raw(path, &st) == 0) {
        /* Existing file: need write permission on the file itself. */
        if (!vfs_access(&st, uid, gid, W_OK)) {
            set_error("permission denied");
            return -1;
        }
    } else {
        /* Doesn't exist yet: some drivers (e.g. FAT16) create it on write,
         * which is really a modification of the parent directory. */
        if (check_dir_write(path, uid, gid) != 0) return -1;
    }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->write_file || m->ops->write_file(sub, data, size, flags) != 0) {
        set_error("write failed");
        return -1;
    }
    return 0;
}

int vfs_create(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;
    if (check_dir_write(path, uid, gid) != 0) return -1;

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->create || m->ops->create(sub, false) != 0) {
        set_error("create failed");
        return -1;
    }
    return 0;
}

int vfs_mkdir(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;
    if (check_dir_write(path, uid, gid) != 0) return -1;

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->create || m->ops->create(sub, true) != 0) {
        set_error("mkdir failed");
        return -1;
    }
    return 0;
}

int vfs_unlink(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;
    if (check_dir_write(path, uid, gid) != 0) return -1;

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->unlink || m->ops->unlink(sub, false) != 0) {
        set_error("unlink failed");
        return -1;
    }
    return 0;
}

int vfs_rmdir(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(path, uid, gid) != 0) return -1;
    if (check_dir_write(path, uid, gid) != 0) return -1;

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->unlink || m->ops->unlink(sub, true) != 0) {
        set_error("rmdir failed");
        return -1;
    }
    return 0;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    uid_t uid = current_uid(), gid = current_gid();
    if (check_traversal(old_path, uid, gid) != 0) return -1;
    if (check_traversal(new_path, uid, gid) != 0) return -1;
    if (check_dir_write(old_path, uid, gid) != 0) return -1;
    if (check_dir_write(new_path, uid, gid) != 0) return -1;

    char old_sub[PUREUNIX_MAX_PATH], new_sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m_old = vfs_dispatch(old_path, old_sub, sizeof(old_sub));
    const vfs_mount_t *m_new = vfs_dispatch(new_path, new_sub, sizeof(new_sub));

    /* Renames only make sense within a single mounted filesystem. */
    if (!m_old || m_old != m_new || !m_old->ops->rename ||
        m_old->ops->rename(old_sub, new_sub) != 0) {
        set_error("rename failed");
        return -1;
    }
    return 0;
}

const char *vfs_last_error(void)
{
    return last_error;
}

/* ---- Permission engine ---- */

bool vfs_access(const vfs_stat_t *st, uid_t uid, gid_t gid, int requested)
{
    if (!st) return false;
    if (requested == F_OK) return true;   /* existence only; caller already confirmed it */

    mode_t mode = st->st_mode;

    if (uid == 0) {
        /* Traditional Unix root behaviour: read/write always allowed;
         * execute allowed only if the file has an execute bit for someone. */
        if ((requested & X_OK) && !(mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            return false;
        }
        return true;
    }

    /* Owner permissions first; otherwise group; otherwise other. Never
     * combined — matching the owner means only the owner bits apply, even
     * if group/other would have granted more. */
    int bits;
    if (st->st_uid == uid) {
        bits = (mode >> 6) & 07;
    } else if (st->st_gid == gid) {
        bits = (mode >> 3) & 07;
    } else {
        bits = mode & 07;
    }

    int req_bits = 0;
    if (requested & R_OK) req_bits |= 04;
    if (requested & W_OK) req_bits |= 02;
    if (requested & X_OK) req_bits |= 01;

    return (bits & req_bits) == req_bits;
}

/* ---- chmod/chown: syscall infrastructure only ----
 * No driver populates ops->chmod/ops->chown yet (EXT2 is read-only; FAT16
 * has no permission storage), so every existing path currently resolves to
 * -EROFS. A future writable filesystem plugs in by filling those ops table
 * slots — nothing here needs to change. */

int vfs_chmod(const char *path, mode_t mode)
{
    (void)mode;
    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m) return -ENOENT;
    vfs_stat_t st;
    if (!m->ops->stat || m->ops->stat(sub, &st) != 0) return -ENOENT;
    if (!m->ops->chmod) return -EROFS;
    return m->ops->chmod(sub, mode);
}

int vfs_chown(const char *path, uid_t uid, gid_t gid)
{
    (void)uid; (void)gid;
    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m) return -ENOENT;
    vfs_stat_t st;
    if (!m->ops->stat || m->ops->stat(sub, &st) != 0) return -ENOENT;
    if (!m->ops->chown) return -EROFS;
    return m->ops->chown(sub, uid, gid);
}

/* ---- Path normalisation (unchanged) ---- */

static void append_component(char *out, const char *component)
{
    if (strcmp(component, ".") == 0 || component[0] == '\0') return;
    if (strcmp(component, "..") == 0) {
        char *slash = strrchr(out, '/');
        if (slash && slash != out) *slash = '\0';
        else strcpy(out, "/");
        return;
    }
    if (strcmp(out, "/") != 0) strcat(out, "/");
    strcat(out, component);
}

void vfs_normalize(char *out, const char *cwd, const char *path)
{
    char  tmp[PUREUNIX_MAX_PATH];
    char *save = NULL;

    if (!path || !*path) {
        strncpy(out, cwd ? cwd : "/", PUREUNIX_MAX_PATH - 1);
        out[PUREUNIX_MAX_PATH - 1] = '\0';
        return;
    }
    if (path[0] == '/') {
        strcpy(out, "/");
        strncpy(tmp, path + 1, sizeof(tmp) - 1);
    } else {
        strncpy(out, cwd && *cwd ? cwd : "/", PUREUNIX_MAX_PATH - 1);
        out[PUREUNIX_MAX_PATH - 1] = '\0';
        strncpy(tmp, path, sizeof(tmp) - 1);
    }
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *tok = strtok_r(tmp, "/", &save); tok;
              tok = strtok_r(NULL, "/", &save)) {
        append_component(out, tok);
    }
    if (out[0] == '\0') strcpy(out, "/");
}
