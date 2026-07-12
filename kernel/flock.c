/* kernel/flock.c — POSIX advisory record locking (fcntl F_SETLK/F_GETLK).
 * See include/pureunix/flock.h for the design (path+owner keying, why no
 * blocking path exists). A small fixed table, this project's usual style
 * for a resource that comes and goes far less often than opens/closes
 * (user/newlib_syscalls.c's popen() pid table is the same shape). */
#include <pureunix/config.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/flock.h>
#include <pureunix/string.h>

#define MAX_FLOCKS 64

typedef struct flock_entry {
    bool used;
    char path[PUREUNIX_MAX_PATH];
    const void *owner;
    int16_t type; /* PU_F_RDLCK or PU_F_WRLCK only — an unlocked range just
                   * isn't stored. */
    int32_t start;
    int32_t len; /* 0 means "to EOF", matching POSIX struct flock. */
} flock_entry_t;

static flock_entry_t g_flocks[MAX_FLOCKS];

static int32_t lock_end(int32_t start, int32_t len)
{
    return len == 0 ? INT32_MAX : start + len;
}

static bool ranges_overlap(int32_t s1, int32_t l1, int32_t s2, int32_t l2)
{
    return s1 < lock_end(s2, l2) && s2 < lock_end(s1, l1);
}

/* A write request conflicts with any overlapping lock; a read request only
 * conflicts with an overlapping write lock — same rule real POSIX uses. */
static bool conflicts(int16_t req_type, int16_t held_type)
{
    return req_type == PU_F_WRLCK || held_type == PU_F_WRLCK;
}

static flock_entry_t *find_conflict(const char *path, const void *owner,
                                     int16_t type, int32_t start, int32_t len)
{
    for (int i = 0; i < MAX_FLOCKS; i++) {
        flock_entry_t *e = &g_flocks[i];
        if (!e->used || e->owner == owner) {
            continue;
        }
        if (strcmp(e->path, path) != 0) {
            continue;
        }
        if (!ranges_overlap(e->start, e->len, start, len)) {
            continue;
        }
        if (conflicts(type, e->type)) {
            return e;
        }
    }
    return NULL;
}

/* Drops every lock this owner holds on `path` overlapping [start, start+len)
 * — used both by an explicit F_UNLCK and to replace an owner's own prior
 * overlapping range before installing a new one (a real fcntl() from the
 * same owner always replaces rather than stacking). */
static void release_overlapping(const char *path, const void *owner,
                                 int32_t start, int32_t len)
{
    for (int i = 0; i < MAX_FLOCKS; i++) {
        flock_entry_t *e = &g_flocks[i];
        if (e->used && e->owner == owner && strcmp(e->path, path) == 0 &&
            ranges_overlap(e->start, e->len, start, len)) {
            e->used = false;
        }
    }
}

int flock_setlk(const char *path, const void *owner, int16_t type, int32_t start, int32_t len)
{
    if (type == PU_F_UNLCK) {
        release_overlapping(path, owner, start, len);
        return 0;
    }

    if (find_conflict(path, owner, type, start, len)) {
        return -EAGAIN;
    }

    release_overlapping(path, owner, start, len);

    for (int i = 0; i < MAX_FLOCKS; i++) {
        if (!g_flocks[i].used) {
            g_flocks[i].used = true;
            strncpy(g_flocks[i].path, path, sizeof(g_flocks[i].path) - 1);
            g_flocks[i].path[sizeof(g_flocks[i].path) - 1] = '\0';
            g_flocks[i].owner = owner;
            g_flocks[i].type = type;
            g_flocks[i].start = start;
            g_flocks[i].len = len;
            return 0;
        }
    }
    return -ENOLCK;
}

int flock_getlk(const char *path, const void *owner, int16_t *type, int32_t *start, int32_t *len)
{
    flock_entry_t *e = find_conflict(path, owner, *type, *start, *len);
    if (e) {
        *type = e->type;
        *start = e->start;
        *len = e->len;
    } else {
        *type = PU_F_UNLCK;
    }
    return 0;
}

void flock_release_owner(const void *owner)
{
    for (int i = 0; i < MAX_FLOCKS; i++) {
        if (g_flocks[i].used && g_flocks[i].owner == owner) {
            g_flocks[i].used = false;
        }
    }
}
