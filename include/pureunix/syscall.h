#ifndef PUREUNIX_SYSCALL_H
#define PUREUNIX_SYSCALL_H

enum {
    SYS_EXIT   = 1,
    SYS_WRITE  = 2,
    SYS_READ   = 3,
    SYS_GETPID = 4,
    SYS_YIELD  = 5,
    SYS_OPEN   = 6,
    SYS_CLOSE  = 7,
    SYS_LSEEK  = 8,
    SYS_STAT   = 9,
    SYS_ACCESS = 10,
    SYS_CHMOD  = 11,
    SYS_CHOWN  = 12,
    SYS_READDIR = 13,
    /* Stage 3A test-only hook: sets the *calling task's* uid/gid outright,
     * with no privilege check whatsoever. This is not a real setuid() —
     * there is no login system or privilege model yet to enforce against.
     * It exists solely so user/ext2test.c can exercise owner/group/other
     * permission logic while running as non-root; nothing outside the
     * regression suite should ever call it. See ext2test.c section [22]. */
    SYS_DEBUG_SETCRED = 14,

    /* Stage 4: symlinks, hard links, and a writable EXT2. */
    SYS_READLINK = 15,
    SYS_LSTAT    = 16,
    SYS_MKDIR    = 17,
    SYS_UNLINK   = 18,
    SYS_RMDIR    = 19,
    SYS_RENAME   = 20,
    SYS_LINK     = 21,
    SYS_SYMLINK  = 22,

    /* Per-process address spaces: duplicate (SYS_FORK), replace-in-place
     * (SYS_EXEC), and reap (SYS_WAIT) a process. See docs/syscalls.md. */
    SYS_FORK     = 23,
    SYS_EXEC     = 24,
    SYS_WAIT     = 25,

    /* Terminal control: get/set the console's struct termios. See
     * include/pureunix/termios.h and drivers/tty.c. */
    SYS_TCGETATTR = 26,
    SYS_TCSETATTR = 27,

    /* Device control. Currently supports exactly one request (TIOCGWINSZ)
     * on the console tty — see include/pureunix/ioctl.h. There is no
     * separate isatty() syscall: userspace derives it from whether
     * SYS_TCGETATTR succeeds (see pu_isatty() in user/libpure.c). */
    SYS_IOCTL = 28,

    /* Per-task working directory. See docs/syscalls.md. */
    SYS_CHDIR  = 29,
    SYS_GETCWD = 30,

    /* Blocking sleep, backed by the PIT tick counter (arch/i386/pit.c).
     * See docs/syscalls.md. */
    SYS_NANOSLEEP = 31,

    /* Read-only credential getters — the write side is SYS_DEBUG_SETCRED
     * (test-only, no privilege check) until a real login/setuid model
     * exists. No separate "effective" uid/gid: PureUNIX has no setuid
     * model, so effective always equals real (see user/newlib_syscalls.c's
     * geteuid()/getegid(), which just alias these). See docs/syscalls.md. */
    SYS_GETUID = 32,
    SYS_GETGID = 33,

    /* Sets a file's atime/mtime directly (EBX: path, ECX: atime, EDX:
     * mtime — each a Unix epoch second count, or 0xFFFFFFFF to leave that
     * one unchanged). Real on EXT2 (fs/ext2/mount.c's ext2_utime()); same
     * -EROFS-on-FAT16 story as SYS_CHMOD/SYS_CHOWN. See docs/syscalls.md. */
    SYS_UTIME = 34,

    /* Wall-clock time as a Unix epoch second count (EBX: pointer to a
     * uint32_t to receive it) — a thin userspace-visible wrapper around
     * kernel/time.c's time_now(), which every write-path timestamp
     * already uses internally. No sub-second resolution exists. See
     * docs/syscalls.md. */
    SYS_GETTIMEOFDAY = 35,

    /* pipe()/dup()/dup2() — see include/pureunix/task.h's open_file_t and
     * docs/syscalls.md. */
    SYS_PIPE = 36,
    SYS_DUP  = 37,
    SYS_DUP2 = 38,

    /* Terminates another task with a signal's POSIX default action — see
     * kernel/task.c's task_kill() and docs/syscalls.md. */
    SYS_KILL = 39,

    /* Minimal fcntl(): F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD only — see
     * arch/i386/syscall.c and docs/syscalls.md. */
    SYS_FCNTL = 40,

    /* Truncate an already-open fd to a given length — see arch/i386/syscall.c
     * and docs/syscalls.md. */
    SYS_FTRUNCATE = 41,

    /* Parent task id — see arch/i386/syscall.c and docs/syscalls.md. */
    SYS_GETPPID = 42,

    /* Sends one ICMP echo request and waits for the matching reply — a
     * thin wrapper around net/icmp.c's icmp_ping(), exposing the kernel's
     * already-complete IPv4/ICMP stack to userspace (see user/ping.c).
     * EBX: destination IPv4 address (ip4_addr_t, host byte order --
     *      include/pureunix/inet.h's IP4_ADDR() convention).
     * ECX: timeout in milliseconds.
     * EDX: optional (may be 0/NULL) pointer to a uint32_t that receives
     *      the round-trip time in milliseconds on success.
     * Returns 0 on a received reply, -ETIMEDOUT if none arrived in time.
     * See arch/i386/syscall.c and docs/syscalls.md. */
    SYS_PING = 43,

    /* fstat(2): metadata for an already-open fd, without needing (or
     * re-resolving) a path. EBX: fd. ECX: struct pureunix_stat *.
     * st_size reflects the open file description's *live* in-memory size
     * (f->size), not a possibly-stale on-disk value, matching real
     * fstat() semantics for a writable fd whose data hasn't been flushed
     * yet. Returns 0 or a negative error. See arch/i386/syscall.c and
     * docs/syscalls.md. */
    SYS_FSTAT = 44,

    /* Process groups and sessions (POSIX setpgid()/getpgid()/setsid()/
     * getsid()) — see kernel/task.c's task_setpgid()/task_getpgid()/
     * task_setsid()/task_getsid() and docs/process-management.md.
     * tcgetpgrp()/tcsetpgrp() are deliberately *not* separate syscalls —
     * they ride on the existing SYS_IOCTL (TIOCGPGRP/TIOCSPGRP, see
     * include/pureunix/ioctl.h), the same way VT_GETACTIVE/VT_ACTIVATE
     * do, rather than growing the syscall table for what's really just
     * another device-control request against a tty fd. */
    SYS_SETPGID = 45, /* EBX: pid (0 == caller), ECX: pgid (0 == "use pid") */
    SYS_GETPGID = 46, /* EBX: pid (0 == caller) */
    SYS_SETSID  = 47, /* no args */
    SYS_GETSID  = 48, /* EBX: pid (0 == caller) */

    /* Signals — see include/pureunix/signal.h's pu_sigaction_t,
     * kernel/signal.c, arch/i386/signal.c, and docs/process-management.md. */
    SYS_SIGACTION   = 49, /* EBX: sig, ECX: const pu_sigaction_t *, EDX: pu_sigaction_t * (old, may be NULL) */
    SYS_SIGPROCMASK = 50, /* EBX: how (SIG_BLOCK/UNBLOCK/SETMASK), ECX: const uint32_t *, EDX: uint32_t * (old, may be NULL) */
    SYS_SIGPENDING  = 51, /* EBX: uint32_t * (out) */

    /* nice()/renice() — kernel/task.c's task_setpriority()/
     * task_getpriority(), docs/process-management.md. */
    SYS_SETPRIORITY = 52, /* EBX: pid (0 == caller), ECX: nice value (clamped to [-20,19]) */
    SYS_GETPRIORITY = 53, /* EBX: pid (0 == caller), ECX: int * (out) */

    /* fchmod(2)/fchown(2): same as SYS_CHMOD/SYS_CHOWN but resolve against
     * an already-open fd's own path (open_file_t.path) instead of a
     * caller-supplied one — added for the SQLite port (docs/sqlite-port.md):
     * os_unix.c's unixCreate() copies the main db file's permissions/
     * ownership onto a freshly created rollback-journal file via these.
     * EBX: fd. ECX (SYS_FCHMOD): mode_t. ECX/EDX (SYS_FCHOWN): uid/gid. */
    SYS_FCHMOD = 54,
    SYS_FCHOWN = 55,

    /* SDL2 platform support (docs/sdl-port.md) -- deliberately dedicated
     * syscalls rather than routed through the VFS/fd model (no
     * FD_KIND_INPUT device node, no /dev/fb): every existing "device
     * special file" (/dev/tty, /dev/null) still represents something a
     * real read()/write()/close() lifecycle makes sense for, but polling
     * raw input events and bulk-blitting a whole framebuffer are both
     * one-shot, fixed-shape operations with nothing to open or close --
     * the same reasoning SYS_PING (net/icmp.c) already applies here.
     *
     * Non-blocking pop from the calling task's own VT's raw input queue
     * (include/pureunix/vt.h's vt_raw_input_push_key()/mouse variants).
     * EBX: struct pureunix_input_event * (out). Returns 1 if an event was
     * popped, 0 if the queue was empty, or a negative errno. */
    SYS_INPUT_POLL = 56,

    /* Framebuffer geometry for whichever VT the caller belongs to (the
     * hardware framebuffer is one physical display shared by every VT --
     * see drivers/framebuffer.c). EBX: struct pureunix_fb_info * (out).
     * Returns 0, or -ENODEV if there is no framebuffer at all (legacy VGA
     * text mode). */
    SYS_FB_GETINFO = 57,

    /* Bulk-copies a whole userspace pixel buffer into the real framebuffer
     * (drivers/framebuffer.c's fb_fill_rect(), row by row, honoring the
     * hardware pitch) -- the SDL pureunix video backend's window surface
     * flip. EBX: const void * (RGB/BGR packed pixels, tightly packed at
     * width*bypp per row -- *not* the hardware's own pitch, translated
     * here). ECX: length in bytes (must exactly equal width*height*bypp).
     * Only has any visible effect while the caller's VT is both active and
     * in graphics mode (SYS_FB_SET_GRAPHICS_MODE) -- otherwise it still
     * succeeds (so a backgrounded SDL app's render loop doesn't need to
     * special-case this), it just doesn't touch real hardware, mirroring
     * vt_write()'s existing backgrounded-VT behavior. Returns 0, or a
     * negative errno (-ENODEV: no framebuffer; -EINVAL: wrong length). */
    SYS_FB_BLIT = 58,

    /* Milliseconds since boot, derived from the PIT tick counter
     * (arch/i386/pit.c's pit_ticks(), 100Hz -- so this has 10ms real
     * resolution despite the millisecond unit) -- SDL_GetTicks()'s entire
     * backing store. Returned directly as this syscall's result (like
     * SYS_GETPID), not through an out-param: unlike SYS_GETTIMEOFDAY this
     * can never need more than 32 bits for any boot session that matters
     * (49 days), and every other free-standing counter syscall here
     * (SYS_GETPID, SYS_GETUID, ...) already returns its value directly. */
    SYS_GET_TICKS_MS = 59,

    /* Toggles the calling task's own VT between text mode (console output
     * repaints the framebuffer normally -- drivers/vga.c) and graphics
     * mode (repaints are suspended so an SDL app can own the framebuffer
     * exclusively via SYS_FB_BLIT without the console clobbering it, or
     * vice versa) -- see kernel/vt.c's vt_set_graphics_mode(). EBX: 1 to
     * enter graphics mode, 0 to leave it (which forces a full text
     * repaint if the VT is still active, restoring the console cleanly --
     * see docs/sdl-port.md's "clean exit" requirement). Returns 0. */
    SYS_SET_GRAPHICS_MODE = 60,

    /* Maps this task's on-demand window-surface pixel buffer at a fixed VA
     * (FB_SHADOW_VA, include/pureunix/vmm.h) within the widened user
     * window, sized to the real framebuffer's width*height*bypp (tightly
     * packed, matching what SYS_FB_BLIT already expects). Deliberately not
     * backed by the newlib heap array or any ELF segment -- see
     * NEWLIB_HEAP_SIZE's comment in user/newlib_syscalls.c for why a
     * shared, always-linked-in allocation would cost every newlib program
     * that many extra real MiB regardless of whether it's an SDL app.
     * Idempotent: a second call from the same task just returns the same
     * VA again (task_t.fb_shadow_mapped) rather than re-allocating.
     * Returns FB_SHADOW_VA (never NULL/0 on success — that address is
     * never 0), or a negative errno (-ENODEV: no framebuffer; -ENOMEM). */
    SYS_FB_MMAP = 61,

    /* Real sbrk(): grows (or, if EBX is negative, shrinks) the calling
     * task's heap break by EBX bytes and returns the *previous* break
     * (matching POSIX sbrk() exactly — user/newlib_syscalls.c's sbrk() is
     * now a thin wrapper around this, replacing what used to be a plain
     * userspace pointer bump through a fixed-size static array). Real
     * pages are mapped one at a time as the break actually grows past
     * whatever's already backed by physical frames (task_t.heap_mapped),
     * up to HEAP_MAX (include/pureunix/vmm.h) -- see HEAP_VA's own comment
     * for why this replaced the old array. Returns -ENOMEM (not -1, unlike
     * a real libc sbrk() which sets errno instead — this is the raw
     * syscall's own negative-errno convention throughout) if growing would
     * exceed HEAP_MAX, or -EINVAL if EBX would shrink the break below 0. */
    SYS_SBRK = 62,

    /* Creates a real PTY pair (include/pureunix/pty.h, kernel/pty.c) — the
     * general primitive a userspace terminal emulator (PUTerm, docs/
     * pude.md) needs: a detached tty whose "hardware" is another ordinary
     * process instead of one of the 6 physical VTs (kernel/vt.c).
     * EBX: pointer to an int[2] output, [0] = master fd, [1] = slave fd
     * (same shape as SYS_PIPE's int[2], master first since it's the
     * "creating" end conceptually, mirroring a real posix_openpt()+
     * grantpt()+unlockpt()+ptsname()->open() sequence collapsed into one
     * call — there's no /dev/pts filesystem here to name the slave by
     * path). Returns 0 on success, or -EINVAL (null pointer) / -EMFILE
     * (fewer than two free fd slots) / -ENOSPC (kernel/pty.c's fixed pty
     * pool, MAX_PTYS, is full). */
    SYS_PTY_CREATE = 63,

    /* fd-based analog of SYS_UTIME, mirroring how SYS_FCHMOD/SYS_FCHOWN
     * already resolve an fd to its open file's path and defer to the same
     * vfs_chmod()/vfs_chown() a path-based syscall uses -- this does the
     * same for vfs_utime(). Added for futimens()/futimes() (Qt Core's
     * qfilesystemengine_unix.cpp calls futimens() for
     * QFile::setFileTime()), a general POSIX primitive, not Qt-specific.
     * EBX: fd. ECX: atime (seconds). EDX: mtime (seconds). */
    SYS_FUTIME = 64,

    /* Whole-filesystem space/inode usage (vfs_statfs_t, fs/vfs.c's
     * vfs_statfs()) — backs statvfs()/statfs() (user/newlib_syscalls.c),
     * needed for real by Qt Core's QStorageInfo (src/corelib/io/
     * qstorageinfo_unix.cpp) as well as BusyBox's df applet. A general
     * POSIX primitive, not Qt-specific.
     * EBX: path. ECX: vfs_statfs_t * (out). */
    SYS_STATFS = 65,

    /* Installs a real i386 Variant-II TLS (thread-local storage) block for
     * the calling task — see task_t.tls_base's own comment
     * (include/pureunix/task.h) and arch/i386/gdt.c's gdt_set_tls_base()
     * for the full picture. Needed for real by any real C11/C++11
     * `thread_local` storage duration (Qt6's QBindingStorage, used
     * pervasively by QObject construction, is the first PureUnix userspace
     * code that actually needs one — see docs/qt-port.md) — a general
     * userspace-runtime primitive, not Qt-specific.
     * EBX: tp (the self-pointing TCB word address user/newlib_crt0.c's
     * tls_init() already built — this syscall just tells the kernel about
     * it so gdt_set_tls_base() can be replayed on every future context
     * switch back to this task, not just right now). */
    SYS_SET_TLS = 66,

    /* Real poll(2) — the "smallest correct general primitive" docs/
     * qt-port.md section 4 already anticipated needing. Genuinely checks
     * FD_KIND_PIPE readiness against the pipe's own real pipe_buf_t.count
     * (fs/ext2 files/tty/pty/procfs fds still report optimistic "whatever
     * was asked for" readiness, same honest-for-what-it-is fallback as
     * before — no readiness tracking exists for those yet); blocks for
     * real (short repeated pit_sleep() polling, arch/i386/syscall.c) up to
     * the requested timeout when nothing is ready yet, rather than lying
     * about instant readiness. The real, first-necessary caller is Qt
     * Core's own event dispatcher's internal same-process wakeup pipe
     * (QCoreApplication::exec() hung waiting on it before this existed —
     * see docs/qt-port.md) — a general POSIX primitive, not Qt-specific;
     * user/newlib_syscalls.c's poll()/select() are now real wrappers
     * around this instead of the old always-ready userspace stub.
     * EBX: struct pu_raw_pollfd[nfds] (fd/events/revents, marshalled by
     * poll()/select() below — see their own comments for the wire
     * struct's exact layout). ECX: nfds. EDX: timeout_ms (-1 = wait
     * indefinitely for at least one fd to become ready, matching POSIX
     * poll()'s own timeout==-1 convention). Returns the number of fds
     * with a nonzero revents (0 on a real timeout), or -EINVAL. */
    SYS_POLL = 67,

    /* Real AF_UNIX domain sockets (kernel/unix_socket.c) — added for the
     * PCManFM-Qt port's MenuCache dependency (docs/pcmanfm-port.md phase
     * 6): its real menu-cache-daemon listens on a path-bound socket and
     * spawns/serves separate client processes over it, a genuine
     * desktop-session IPC pattern this project had never needed a real
     * kernel primitive for before (every earlier "socket" reference
     * across this codebase was an honest ENOSYS stub — see
     * user/newlib_compat/sys/socket.h's own long-standing comment).
     * SOCK_STREAM only (nothing in this port needs SOCK_DGRAM); AF_INET/
     * AF_INET6 remain honest ENOSYS — this is local-IPC-only, not a real
     * network stack. Modeled directly on the existing SYS_PIPE/
     * FD_KIND_PIPE machinery: a connected pair is just two pipe_buf_t
     * ring buffers (one per direction) instead of pipe()'s one, plus a
     * small fixed-size path registry so unrelated processes (not just
     * fork() relatives, unlike pipes) can rendezvous by path. */

    /* EBX: domain (AF_UNIX only). ECX: type (SOCK_STREAM only). EDX:
     * protocol (must be 0). Returns a new fd for an unbound, unconnected
     * socket, or -EAFNOSUPPORT/-EPROTOTYPE for anything else requested. */
    SYS_SOCKET = 68,
    /* EBX: fd. ECX: const struct sockaddr_un *. EDX: addrlen. Registers
     * the socket in the real fixed path registry (kernel/unix_socket.c) —
     * -EADDRINUSE if that path is already bound by a live listening
     * socket, -EINVAL if already bound/connected. */
    SYS_BIND = 69,
    /* EBX: fd. ECX: backlog (clamped to the real fixed backlog array
     * size). Marks a bound socket as listening; -EDESTADDRREQ if never
     * bound. */
    SYS_LISTEN = 70,
    /* EBX: fd (a listening socket). Blocks (wait_queue_sleep, same
     * primitive as SYS_PIPE's blocked read/write) until a real pending
     * connect() is queued, then installs a new connected socket fd in
     * the *calling* task's own fd table and returns it — real POSIX
     * accept() semantics, not merely returning the listener's own fd
     * again. -EINVAL if not listening. */
    SYS_ACCEPT = 71,
    /* EBX: fd (a fresh, unconnected socket). ECX: const struct
     * sockaddr_un *. EDX: addrlen. Real kernel-mediated rendezvous, not a
     * network handshake: allocates the two pipe_buf_t rings, completes
     * this end's connection immediately, and queues the peer half for
     * the listener's next SYS_ACCEPT to pick up — matching real AF_UNIX
     * behavior, where connect() returns as soon as the kernel accepts it
     * into the listen backlog, without waiting for the peer's own
     * accept() call to actually happen first. -ECONNREFUSED if no
     * listening socket is registered at that path. */
    SYS_CONNECT = 72,
};

#endif
