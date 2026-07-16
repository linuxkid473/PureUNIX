// PureUnix's qplatformdefs.h — see qmake.conf's header comment for context.
//
// Deliberately does NOT include pthread.h, dlfcn.h, sys/socket.h,
// sys/shm.h, sys/ipc.h, grp.h, pwd.h, or netinet/in.h the way
// mkspecs/common/bsd/qplatformdefs.h does: none of those exist on
// PureUnix (no threading, no dynamic loading, no sockets, no SysV IPC,
// no real multi-user group/passwd database - see docs/qt-port.md
// sections 2-4). Only what the vendored newlib (third_party/newlib)
// genuinely provides is included here.
#ifndef QPLATFORMDEFS_H
#define QPLATFORMDEFS_H

#include "qglobal.h"

// No <sys/socket.h> on this target - QT_NO_SOCKET_H skips the one
// conditional socket.h include inside ../posix/qplatformdefs.h; the
// QT_SOCKLEN_T/QT_SOCKET_CONNECT/QT_SOCKET_BIND macros it still defines
// unconditionally are simply never used with FEATURE_network=OFF.
#define QT_NO_SOCKET_H

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "../common/posix/qplatformdefs.h"

#undef QT_OPEN_LARGEFILE
#define QT_OPEN_LARGEFILE       0

#define QT_SNPRINTF             ::snprintf
#define QT_VSNPRINTF            ::vsnprintf

#endif // QPLATFORMDEFS_H
