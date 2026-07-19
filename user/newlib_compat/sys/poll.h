/* Override for glibc/Linux's <sys/poll.h> (a plain forwarding header
 * there over <poll.h> since ~forever), which newlib doesn't have at all
 * — real POSIX only requires <poll.h>. GLib's own meson.build
 * (docs/pcmanfm-port.md phase 3/6) hard-requires cc.has_header('sys/
 * poll.h') to succeed (its only two recognized cases are this or
 * <winsock2.h> for Windows — a real gap in its own cross-platform
 * detection, not something to patch there when the real, minimal fix is
 * exactly this file, the same "compat header shadows a location newlib
 * never provided" technique already used throughout
 * user/newlib_compat/). This platform's own <poll.h> (also in
 * user/newlib_compat/) already declares everything real POSIX poll()
 * needs — just forward to it.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_POLL_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_POLL_H

#include <poll.h>

#endif
