/* Override for <utime.h> — newlib's own real copy (third_party/newlib/
 * i686-elf/include/utime.h) only #includes <sys/utime.h> for
 * `struct utimbuf`, and that header is explicitly the generic "dummy,
 * not customized for any particular system" fallback (its own comment:
 * "If there is a utime.h in libc/sys/SYSDIR/sys, it will override this
 * one" — no real per-target port ever did that for this vendored
 * newlib), so utime() itself is never actually declared anywhere. GLib's
 * glib/gstdio.c (docs/pcmanfm-port.md phase 3/6) calls it directly.
 * Real function (user/newlib_syscalls.c) — a real, working conversion
 * on top of the already-real utimes(), not a stub.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_UTIME_H
#define PUREUNIX_NEWLIB_COMPAT_UTIME_H

#include <sys/utime.h>

#ifdef __cplusplus
extern "C" {
#endif

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif
#endif
