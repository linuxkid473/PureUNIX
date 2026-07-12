#ifndef PUREUNIX_FLOCK_H
#define PUREUNIX_FLOCK_H

#include <pureunix/types.h>

/* POSIX advisory record locking — backs fcntl(F_SETLK/F_GETLK) (SYS_FCNTL,
 * arch/i386/syscall.c) for FD_KIND_FILE descriptors. Added for the SQLite
 * port (docs/sqlite-port.md): SQLite's default unix VFS (os_unix.c)
 * unconditionally probes/acquires PENDING/RESERVED/SHARED/EXCLUSIVE byte-
 * range locks via fcntl on every transaction, even with a single
 * connection — without a real F_SETLK, every SQLite operation fails with
 * an I/O-locking error before doing anything else.
 *
 * Locks are keyed by (path, owner) rather than (device, inode, pid), two
 * deliberate simplifications from real POSIX semantics:
 *
 *  - No st_dev/st_ino: PureUNIX's vfs_stat_t has no st_dev, and keying by
 *    path alone is indistinguishable from keying by inode for every
 *    realistic SQLite use (a database file opened by its own path, never
 *    hardlinked). Two different paths to the same inode would incorrectly
 *    get independent lock spaces — an accepted, documented gap, not an
 *    oversight.
 *  - `owner` is the open_file_t* holding the lock, not a pid. Real POSIX
 *    locking is scoped per-*process* (closing *any* fd on an inode drops
 *    every lock that process holds on it via any fd) specifically because
 *    a single process can have many independent fds on the same file;
 *    SQLite's own os_unix.c already works around exactly that surprising
 *    rule with its own per-process unixInodeInfo bookkeeping layer so it
 *    only ever calls fcntl() when a real OS-visible state change is
 *    needed. Since every PureUNIX open() produces its own open_file_t and
 *    a SQLite connection keeps exactly one open for a given db file's
 *    entire lifetime, "owner" and "process" coincide for every case this
 *    port needs; open_file_unref() (kernel/task.c) releases every lock a
 *    closing open_file_t held, which is both simpler than real per-pid
 *    scoping and correct for this use.
 *
 * SQLite's default build (no SQLITE_ENABLE_SETLK_TIMEOUT) never issues a
 * blocking F_SETLKW — every lock attempt is F_SETLK, with SQLITE_BUSY
 * retry handled entirely at the SQL layer (the busy-handler callback) —
 * so this deliberately has no blocking/wait-queue path at all. */

int flock_setlk(const char *path, const void *owner, int16_t type, int32_t start, int32_t len);
int flock_getlk(const char *path, const void *owner, int16_t *type, int32_t *start, int32_t *len);
/* Called from open_file_unref() (kernel/task.c) when a FD_KIND_FILE
 * open_file_t's last reference goes away — drops every lock it held. */
void flock_release_owner(const void *owner);

#endif
