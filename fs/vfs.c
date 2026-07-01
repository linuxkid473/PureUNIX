#include <pureunix/ext2.h>
#include <pureunix/fat16.h>
#include <pureunix/string.h>
#include <pureunix/vfs.h>

/*
 * VFS dispatch layer.
 *
 * EXT2 is the primary root filesystem: every path is served by EXT2 unless
 * it falls under the "/fat" mount point, in which case it is routed to
 * FAT16 (kept mounted for compatibility/testing only). This is a simple
 * mount-point router, not a union — a path is served by exactly one
 * filesystem.
 *
 * EXT2 is read-only (Stage 1/2A), so all write operations are routed to
 * FAT16 and only work under "/fat".
 */

#define FAT_MOUNT_PREFIX "/fat"

static char last_error[96] = "no error";

static void set_error(const char *msg)
{
    strncpy(last_error, msg, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

/*
 * If path is under the "/fat" mount point, write the path FAT16 should see
 * (with the "/fat" prefix stripped, root normalized to "/") into out and
 * return true. Otherwise return false so the caller dispatches to EXT2.
 */
static bool route_fat16(const char *path, char *out, size_t out_size)
{
    size_t prefix_len = sizeof(FAT_MOUNT_PREFIX) - 1;

    if (strncmp(path, FAT_MOUNT_PREFIX, prefix_len) != 0) return false;
    char next = path[prefix_len];
    if (next != '\0' && next != '/') return false;

    const char *rest = path + prefix_len;
    if (rest[0] == '\0' || strcmp(rest, "/") == 0) {
        strncpy(out, "/", out_size - 1);
    } else {
        strncpy(out, rest, out_size - 1);
    }
    out[out_size - 1] = '\0';
    return true;
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
    if (ext2_is_mounted()) {
        set_error("no error");
        return 0;
    }
    set_error("no filesystem mounted");
    return -1;
}

/* ---- Read dispatch: mount-point routing, no union ---- */

int vfs_stat(const char *path, vfs_stat_t *st)
{
    char sub[PUREUNIX_MAX_PATH];

    if (route_fat16(path, sub, sizeof(sub))) {
        if (fat16_is_mounted() && fat16_stat(sub, st) == 0) return 0;
        set_error("stat failed");
        return -1;
    }

    if (ext2_is_mounted() && ext2_stat(path, st) == 0) return 0;
    set_error("stat failed");
    return -1;
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    char sub[PUREUNIX_MAX_PATH];

    if (route_fat16(path, sub, sizeof(sub))) {
        if (fat16_is_mounted() && fat16_read_file(sub, out_data, out_size) == 0) return 0;
        set_error("read failed");
        return -1;
    }

    if (ext2_is_mounted() && ext2_read_file(path, out_data, out_size) == 0) return 0;
    set_error("read failed");
    return -1;
}

int vfs_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    char sub[PUREUNIX_MAX_PATH];

    if (route_fat16(path, sub, sizeof(sub))) {
        if (fat16_is_mounted() && fat16_readdir(sub, cb, ctx) == 0) return 0;
        set_error("readdir failed");
        return -1;
    }

    if (ext2_is_mounted() && ext2_readdir(path, cb, ctx) == 0) return 0;
    set_error("readdir failed");
    return -1;
}

/* ---- Write operations: routed to FAT16 under /fat; EXT2 is read-only ---- */

int vfs_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    char sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(path, sub, sizeof(sub)) || !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_write_file(sub, data, size, flags);
    if (r != 0) set_error("write failed");
    return r;
}

int vfs_create(const char *path)
{
    char sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(path, sub, sizeof(sub)) || !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_create(sub, false);
    if (r != 0) set_error("create failed");
    return r;
}

int vfs_mkdir(const char *path)
{
    char sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(path, sub, sizeof(sub)) || !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_create(sub, true);
    if (r != 0) set_error("mkdir failed");
    return r;
}

int vfs_unlink(const char *path)
{
    char sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(path, sub, sizeof(sub)) || !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_unlink(sub, false);
    if (r != 0) set_error("unlink failed");
    return r;
}

int vfs_rmdir(const char *path)
{
    char sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(path, sub, sizeof(sub)) || !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_unlink(sub, true);
    if (r != 0) set_error("rmdir failed");
    return r;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_sub[PUREUNIX_MAX_PATH], new_sub[PUREUNIX_MAX_PATH];
    if (!route_fat16(old_path, old_sub, sizeof(old_sub)) ||
        !route_fat16(new_path, new_sub, sizeof(new_sub)) ||
        !fat16_is_mounted()) {
        set_error("no writable filesystem");
        return -1;
    }
    int r = fat16_rename(old_sub, new_sub);
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
