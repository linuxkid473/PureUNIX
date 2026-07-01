#include <pureunix/ext2.h>
#include <pureunix/fat16.h>
#include <pureunix/string.h>
#include <pureunix/vfs.h>

/*
 * VFS dispatch layer.
 *
 * Read operations try EXT2 first, then fall back to FAT16.  This lets
 * EXT2 serve as the primary data filesystem while FAT16 remains the
 * program store (shells, ELF binaries under /bin/ that only FAT16 has).
 *
 * Write operations (write_file, create, mkdir, unlink, rmdir, rename) are
 * routed exclusively to FAT16; EXT2 Stage 1 is read-only.
 */

static char last_error[96] = "no error";

static void set_error(const char *msg)
{
    strncpy(last_error, msg, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

int vfs_init(void)
{
    set_error("no filesystem mounted");
    return 0;
}

bool vfs_mounted(void)
{
    return ext2_is_mounted() || fat16_is_mounted();
}

int vfs_mount_root(void)
{
    if (ext2_is_mounted() || fat16_is_mounted()) {
        set_error("no error");
        return 0;
    }
    set_error("no filesystem mounted");
    return -1;
}

/* ---- Read dispatch: EXT2 first, FAT16 fallback ---- */

int vfs_stat(const char *path, vfs_stat_t *st)
{
    if (ext2_is_mounted() && ext2_stat(path, st) == 0) return 0;
    if (fat16_is_mounted() && fat16_stat(path, st) == 0) return 0;
    set_error("stat failed");
    return -1;
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    if (ext2_is_mounted() && ext2_read_file(path, out_data, out_size) == 0) return 0;
    if (fat16_is_mounted() && fat16_read_file(path, out_data, out_size) == 0) return 0;
    set_error("read failed");
    return -1;
}

int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    /*
     * Enumerate entries from both filesystems when both have the directory.
     * This is necessary because the two filesystems partition the namespace:
     * EXT2 holds data files, FAT16 holds programs (e.g. /bin).  A directory
     * like "/" exists in both; using either one exclusively hides the other's
     * contents.
     *
     * vfs_stat uses the same "try both" pattern for individual path lookups.
     * readdir must mirror it — inconsistency between the two is the root cause
     * of paths that stat() finds but ls never shows.
     */
    bool served = false;

    if (ext2_is_mounted()) {
        vfs_stat_t st;
        if (ext2_stat(path, &st) == 0 && st.type == VFS_DIR) {
            ext2_readdir(path, cb, ctx);
            served = true;
        }
    }

    if (fat16_is_mounted()) {
        vfs_stat_t st;
        if (fat16_stat(path, &st) == 0 && st.type == VFS_DIR) {
            fat16_readdir(path, cb, ctx);
            served = true;
        }
    }

    if (!served) {
        set_error("readdir failed");
        return -1;
    }
    return 0;
}

/* ---- Write operations: FAT16 only ---- */

int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_write_file(path, data, size, flags);
    if (r != 0) set_error("write failed");
    return r;
}

int vfs_create(const char *path)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_create(path, false);
    if (r != 0) set_error("create failed");
    return r;
}

int vfs_mkdir(const char *path)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_create(path, true);
    if (r != 0) set_error("mkdir failed");
    return r;
}

int vfs_unlink(const char *path)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_unlink(path, false);
    if (r != 0) set_error("unlink failed");
    return r;
}

int vfs_rmdir(const char *path)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_unlink(path, true);
    if (r != 0) set_error("rmdir failed");
    return r;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    if (!fat16_is_mounted()) { set_error("no writable filesystem"); return -1; }
    int r = fat16_rename(old_path, new_path);
    if (r != 0) set_error("rename failed");
    return r;
}

const char *vfs_last_error(void)
{
    return last_error;
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
