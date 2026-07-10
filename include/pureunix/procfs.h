#ifndef PUREUNIX_PROCFS_H
#define PUREUNIX_PROCFS_H

#include <pureunix/vfs.h>

/* fs/procfs.c — a minimal, synthetic /proc, mounted read-only at boot
 * (kernel/main.c). Exists specifically because BusyBox's libbb/procps.c
 * (shared by the ps/top applets) unconditionally does xopendir("/proc")
 * with no non-procfs code path at all — this isn't an optional nicety,
 * `ps`/`top` cannot function without it. See docs/procfs.md for exactly
 * which files exist and why, and docs/process-management.md for the
 * kernel-side process model this exposes. */
const vfs_ops_t *procfs_vfs_ops(void);

#endif
