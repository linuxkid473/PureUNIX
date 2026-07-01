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
 * Stage 3A added Unix permission enforcement at this same layer: drivers
 * hand back raw metadata (vfs_stat_t::st_mode/st_uid/st_gid) and know
 * nothing about who's asking. Every public entry point resolves the
 * calling task's credentials via current_uid()/current_gid() and runs them
 * through vfs_access() (the one central permission engine).
 *
 * Stage 4 adds real pathname resolution: resolve_path() below walks a path
 * component by component, following any symlink it encounters (in every
 * ancestor directory always, and in the final component when the caller
 * asks for it), performing the X_OK traversal check on each directory
 * consulted along the way, and bailing out with -ELOOP after 40 follows.
 * Every public entry point is built on top of this one resolver, so
 * pathname resolution — including symlink-following — exists in exactly
 * one place. From here on every vfs_* entry point returns 0 or a negative
 * errno (matching the convention vfs_chmod/vfs_chown already established),
 * rather than the old bare 0/-1.
 */

#define VFS_MAX_MOUNTS 8
#define MAX_SYMLINK_FOLLOWS 40
#define RESOLVE_WORK_SIZE (PUREUNIX_MAX_PATH * 8)

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

/* Raw stat: dispatch + driver call, no permission checking and no symlink
 * following — used internally by the resolver itself (to inspect each
 * literal path it builds up) so resolution never recurses into itself. */
static int vfs_stat_raw(const char *path, vfs_stat_t *st)
{
    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->stat || m->ops->stat(sub, st) != 0) return -1;
    return 0;
}

/* Raw readlink: dispatch + driver call, no permission checking. A symlink's
 * own mode bits are conventionally ignored everywhere in Unix (they're
 * always rendered as 0777), so reading its target is never gated the way
 * reading a regular file's contents is. */
static int vfs_readlink_raw(const char *path, char *buf, size_t bufsize)
{
    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(path, sub, sizeof(sub));
    if (!m || !m->ops->readlink) return -EINVAL;
    return m->ops->readlink(sub, buf, bufsize);
}

/* ---------------------------------------------------------------------
 * Pathname resolution with symlink-following (Stage 4)
 * ------------------------------------------------------------------ */

/* Append one path component to the resolved-so-far absolute path in
 * `resolved`, handling "." and ".." exactly as append_component() (below,
 * used by vfs_normalize) does. */
static void resolved_append(char *resolved, const char *component)
{
    if (strcmp(component, ".") == 0) return;
    if (strcmp(component, "..") == 0) {
        char *slash = strrchr(resolved, '/');
        if (slash && slash != resolved) *slash = '\0';
        else strcpy(resolved, "/");
        return;
    }
    if (strcmp(resolved, "/") != 0) strcat(resolved, "/");
    strcat(resolved, component);
}

/*
 * Turn path (must be absolute — callers normalize relative shell input via
 * vfs_normalize() first) into a fully symlink-resolved absolute path in
 * out. Every ancestor directory is followed through any symlink it names;
 * the final component is followed too iff follow_final is true. X_OK is
 * required on every directory actually consulted (root bypasses via
 * vfs_access, as always). A dangling final component (one that doesn't
 * exist on disk) is not an error here — resolution succeeds with the
 * would-be path in out, and it is each *caller's* job to vfs_stat_raw() it
 * and decide whether non-existence is expected (create/mkdir/symlink) or
 * fatal (open/read/unlink/...).
 *
 * Returns 0 on success, or a negative errno: -ENOENT (an ancestor doesn't
 * exist), -EACCES (missing X_OK on a directory along the way), -ELOOP
 * (more than 40 symlink follows), -ENAMETOOLONG (pathological expansion).
 */
static int resolve_path(const char *path, char *out, size_t out_size,
                        bool follow_final, uid_t uid, gid_t gid)
{
    if (!path || path[0] != '/') return -ENOENT;

    char work[RESOLVE_WORK_SIZE];
    if (strlen(path + 1) >= sizeof(work)) return -ENAMETOOLONG;
    strcpy(work, path + 1);

    char resolved[PUREUNIX_MAX_PATH];
    strcpy(resolved, "/");

    int follows = 0;
    char *cursor = work;

    while (*cursor) {
        char *slash = strchr(cursor, '/');
        size_t clen = slash ? (size_t)(slash - cursor) : strlen(cursor);
        char *rest = slash ? slash + 1 : cursor + clen;

        if (clen == 0) { /* empty component from "//" */
            cursor = rest;
            continue;
        }
        if (clen >= PUREUNIX_MAX_NAME) return -ENAMETOOLONG;

        char component[PUREUNIX_MAX_NAME];
        memcpy(component, cursor, clen);
        component[clen] = '\0';
        bool is_last = (*rest == '\0');
        cursor = rest;

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            resolved_append(resolved, component);
            continue;
        }

        /* X_OK on the directory we're about to search. */
        vfs_stat_t dst;
        if (vfs_stat_raw(resolved, &dst) != 0) return -ENOENT;
        if (!vfs_access(&dst, uid, gid, X_OK)) return -EACCES;

        char candidate[PUREUNIX_MAX_PATH];
        strcpy(candidate, resolved);
        resolved_append(candidate, component);

        vfs_stat_t cst;
        if (vfs_stat_raw(candidate, &cst) != 0) {
            if (is_last) {
                strncpy(resolved, candidate, sizeof(resolved) - 1);
                resolved[sizeof(resolved) - 1] = '\0';
                break;
            }
            return -ENOENT;
        }

        bool should_follow = (cst.type == VFS_SYMLINK) && (!is_last || follow_final);
        if (should_follow) {
            if (++follows > MAX_SYMLINK_FOLLOWS) return -ELOOP;

            char target[PUREUNIX_MAX_PATH];
            int n = vfs_readlink_raw(candidate, target, sizeof(target) - 1);
            if (n < 0) return -ENOENT;
            target[n] = '\0';

            size_t rest_len = strlen(cursor);
            size_t need = strlen(target) + 1 + rest_len + 1;
            if (need > sizeof(work)) return -ENAMETOOLONG;

            char newwork[RESOLVE_WORK_SIZE];
            strcpy(newwork, target);
            if (rest_len > 0) {
                strcat(newwork, "/");
                strcat(newwork, cursor);
            }
            strcpy(work, newwork);
            cursor = work;

            if (target[0] == '/') {
                strcpy(resolved, "/");
                /* target's own leading '/' was already consumed by strcpy
                 * above copying it verbatim into work; strip it back off so
                 * the component loop doesn't see an empty first component. */
                if (work[0] == '/') memmove(work, work + 1, strlen(work));
                cursor = work;
            }
            continue;
        }

        strcpy(resolved, candidate);
    }

    strncpy(out, resolved, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Split path into its parent directory (out). */
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
    if (vfs_stat_raw(parent, &pst) != 0) return -ENOENT;
    if (!vfs_access(&pst, uid, gid, W_OK)) return -EACCES;
    return 0;
}

/* ---- Read dispatch ---- */

int vfs_stat(const char *path, vfs_stat_t *st)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), true, uid, gid);
    if (rc != 0) { set_error("stat failed"); return rc; }
    if (vfs_stat_raw(resolved, st) != 0) { set_error("stat failed"); return -ENOENT; }
    return 0;
}

int vfs_lstat(const char *path, vfs_stat_t *st)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), false, uid, gid);
    if (rc != 0) { set_error("lstat failed"); return rc; }
    if (vfs_stat_raw(resolved, st) != 0) { set_error("lstat failed"); return -ENOENT; }
    return 0;
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), true, uid, gid);
    if (rc != 0) { set_error("read failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) != 0) { set_error("read failed"); return -ENOENT; }
    if (!vfs_access(&st, uid, gid, R_OK)) { set_error("permission denied"); return -EACCES; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->read_file || m->ops->read_file(sub, out_data, out_size) != 0) {
        set_error("read failed");
        return -EIO;
    }
    return 0;
}

int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), true, uid, gid);
    if (rc != 0) { set_error("readdir failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) != 0) { set_error("readdir failed"); return -ENOENT; }
    if (!vfs_access(&st, uid, gid, R_OK)) { set_error("permission denied"); return -EACCES; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->readdir) { set_error("readdir failed"); return -EIO; }
    /* A driver's readdir returns 0 for "visited every entry" or a positive
     * value for "the callback asked to stop early" (e.g. the caller's
     * output buffer filled up) — both are successful completions, not
     * errors, per vfs_readdir_cb_t's "return non-zero to stop iteration"
     * contract. Only a negative return is an actual driver failure (bad
     * inode, not a directory, I/O error). */
    if (m->ops->readdir(sub, cb, ctx) < 0) {
        set_error("readdir failed");
        return -EIO;
    }
    return 0;
}

/* ---- Write dispatch ---- */

int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), true, uid, gid);
    if (rc != 0) { set_error("write failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) == 0) {
        if (!vfs_access(&st, uid, gid, W_OK)) { set_error("permission denied"); return -EACCES; }
    } else {
        rc = check_dir_write(resolved, uid, gid);
        if (rc != 0) { set_error("permission denied"); return rc; }
    }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->write_file) { set_error("write failed"); return -EROFS; }
    int wr = m->ops->write_file(sub, data, size, flags);
    if (wr != 0) { set_error("write failed"); return wr < 0 ? wr : -EIO; }
    return 0;
}

/* Shared by vfs_create/vfs_mkdir: resolve path (following symlinks so a
 * name that resolves through a symlink chain to an existing file is
 * correctly rejected as EEXIST), refuse if something is already there,
 * then require W_OK on the real parent directory. */
static int resolve_for_create(const char *path, char *resolved, size_t resolved_size,
                              uid_t uid, gid_t gid)
{
    int rc = resolve_path(path, resolved, resolved_size, true, uid, gid);
    if (rc != 0) return rc;

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) == 0) return -EEXIST;

    return check_dir_write(resolved, uid, gid);
}

int vfs_create(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_for_create(path, resolved, sizeof(resolved), uid, gid);
    if (rc != 0) { set_error("create failed"); return rc; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->create) { set_error("create failed"); return -EROFS; }
    int r = m->ops->create(sub, false);
    if (r != 0) { set_error("create failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_mkdir(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_for_create(path, resolved, sizeof(resolved), uid, gid);
    if (rc != 0) { set_error("mkdir failed"); return rc; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->create) { set_error("mkdir failed"); return -EROFS; }
    int r = m->ops->create(sub, true);
    if (r != 0) { set_error("mkdir failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_unlink(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), false, uid, gid);
    if (rc != 0) { set_error("unlink failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) != 0) { set_error("unlink failed"); return -ENOENT; }
    if (st.type == VFS_DIR) { set_error("is a directory"); return -EISDIR; }

    rc = check_dir_write(resolved, uid, gid);
    if (rc != 0) { set_error("permission denied"); return rc; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->unlink) { set_error("unlink failed"); return -EROFS; }
    int r = m->ops->unlink(sub, false);
    if (r != 0) { set_error("unlink failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_rmdir(const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), false, uid, gid);
    if (rc != 0) { set_error("rmdir failed"); return rc; }

    if (strcmp(resolved, "/") == 0) { set_error("cannot remove root"); return -EACCES; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) != 0) { set_error("rmdir failed"); return -ENOENT; }
    if (st.type != VFS_DIR) { set_error("not a directory"); return -ENOTDIR; }

    rc = check_dir_write(resolved, uid, gid);
    if (rc != 0) { set_error("permission denied"); return rc; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->unlink) { set_error("rmdir failed"); return -EROFS; }
    int r = m->ops->unlink(sub, true);
    if (r != 0) { set_error("rmdir failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved_old[PUREUNIX_MAX_PATH], resolved_new[PUREUNIX_MAX_PATH];

    int rc = resolve_path(old_path, resolved_old, sizeof(resolved_old), false, uid, gid);
    if (rc != 0) { set_error("rename failed"); return rc; }
    rc = resolve_path(new_path, resolved_new, sizeof(resolved_new), false, uid, gid);
    if (rc != 0) { set_error("rename failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved_old, &st) != 0) { set_error("rename failed"); return -ENOENT; }

    rc = check_dir_write(resolved_old, uid, gid);
    if (rc != 0) { set_error("permission denied"); return rc; }
    rc = check_dir_write(resolved_new, uid, gid);
    if (rc != 0) { set_error("permission denied"); return rc; }

    char old_sub[PUREUNIX_MAX_PATH], new_sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m_old = vfs_dispatch(resolved_old, old_sub, sizeof(old_sub));
    const vfs_mount_t *m_new = vfs_dispatch(resolved_new, new_sub, sizeof(new_sub));

    if (!m_old || !m_new) { set_error("rename failed"); return -ENOENT; }
    if (m_old != m_new) { set_error("cross-device rename"); return -EXDEV; }
    if (!m_old->ops->rename) { set_error("rename failed"); return -EROFS; }

    int r = m_old->ops->rename(old_sub, new_sub);
    if (r != 0) { set_error("rename failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_link(const char *old_path, const char *new_path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved_old[PUREUNIX_MAX_PATH];
    int rc = resolve_path(old_path, resolved_old, sizeof(resolved_old), false, uid, gid);
    if (rc != 0) { set_error("link failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved_old, &st) != 0) { set_error("link failed"); return -ENOENT; }
    if (st.type == VFS_DIR) { set_error("cannot link directories"); return -EPERM; }

    char resolved_new[PUREUNIX_MAX_PATH];
    rc = resolve_for_create(new_path, resolved_new, sizeof(resolved_new), uid, gid);
    if (rc != 0) { set_error("link failed"); return rc; }

    char old_sub[PUREUNIX_MAX_PATH], new_sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m_old = vfs_dispatch(resolved_old, old_sub, sizeof(old_sub));
    const vfs_mount_t *m_new = vfs_dispatch(resolved_new, new_sub, sizeof(new_sub));

    if (!m_old || !m_new) { set_error("link failed"); return -ENOENT; }
    if (m_old != m_new) { set_error("cross-device link"); return -EXDEV; }
    if (!m_old->ops->link) { set_error("link failed"); return -EROFS; }

    int r = m_old->ops->link(old_sub, new_sub);
    if (r != 0) { set_error("link failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_symlink(const char *target, const char *path)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_for_create(path, resolved, sizeof(resolved), uid, gid);
    if (rc != 0) { set_error("symlink failed"); return rc; }

    char sub[PUREUNIX_MAX_PATH];
    const vfs_mount_t *m = vfs_dispatch(resolved, sub, sizeof(sub));
    if (!m || !m->ops->symlink) { set_error("symlink failed"); return -EROFS; }
    int r = m->ops->symlink(target, sub);
    if (r != 0) { set_error("symlink failed"); return r < 0 ? r : -EIO; }
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t bufsize)
{
    uid_t uid = current_uid(), gid = current_gid();
    char resolved[PUREUNIX_MAX_PATH];
    int rc = resolve_path(path, resolved, sizeof(resolved), false, uid, gid);
    if (rc != 0) { set_error("readlink failed"); return rc; }

    vfs_stat_t st;
    if (vfs_stat_raw(resolved, &st) != 0) { set_error("readlink failed"); return -ENOENT; }
    if (st.type != VFS_SYMLINK) { set_error("not a symlink"); return -EINVAL; }

    int n = vfs_readlink_raw(resolved, buf, bufsize);
    if (n < 0) { set_error("readlink failed"); return n; }
    return n;
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
 * EXT2 still stores no mutable permission/ownership metadata path (Stage 4
 * only wires up create/write/unlink/rename/link/symlink — chmod/chown
 * remain out of scope per the Stage 4 spec), so these still resolve to
 * -EROFS for FAT16 and EXT2 alike. */

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
