#ifndef PUREUNIX_UNIX_SOCKET_H
#define PUREUNIX_UNIX_SOCKET_H

/* Real AF_UNIX domain sockets (SOCK_STREAM only) — added for the
 * PCManFM-Qt port's MenuCache dependency (docs/pcmanfm-port.md phase 6,
 * see include/pureunix/syscall.h's own SYS_SOCKET/.../SYS_CONNECT
 * comment for the full rationale). Modeled directly on kernel/pty.c: a
 * fixed pool (mirrors MAX_PTYS's own style), real wait-queue blocking
 * (kernel/wait.c) instead of a spin loop.
 *
 * A connected pair is just two pipe_buf_t ring buffers (include/
 * pureunix/task.h — the exact same struct SYS_PIPE already uses), one
 * per direction, instead of pipe()'s one — this end's `tx` is the other
 * end's `rx` and vice versa. Real reuse, not a coincidence: an AF_UNIX
 * SOCK_STREAM connection genuinely behaves exactly like two pipes glued
 * back to back once established; the only real new pieces this file
 * adds on top of that are the small fixed path registry (so unrelated
 * processes — not just fork() relatives, unlike a bare pipe — can
 * rendezvous by path) and the listen backlog/accept queue. */
#include <pureunix/task.h>
#include <pureunix/types.h>
#include <pureunix/wait.h>

typedef struct unix_socket unix_socket_t;

/* Allocates a fresh, unbound, unconnected socket from the fixed pool
 * (mirrors pty_alloc()'s own style/comment). Starts at refcount 1 — the
 * caller (SYS_SOCKET) installs exactly one open_file_t referencing it,
 * same convention as open_file_alloc(). Returns NULL if the pool is
 * exhausted. */
unix_socket_t *unix_socket_alloc(void);

/* Bumps refcount (dup()/dup2()/fork() sharing one open_file_t bump
 * open_file_t.refcount instead — see task.h's own note on pipe_buf_t —
 * this is only for symmetry with pty_master_ref(); SYS_SOCKET/SYS_ACCEPT
 * are the only real callers, exactly once each). */
void unix_socket_ref(unix_socket_t *s);
/* Drops a reference; wakes the connected peer's blocked reader/writer
 * (EOF/EPIPE, exactly like pipe close) once the last reference on a
 * *connected* socket goes away, or simply frees a *listening* socket's
 * pool slot (any not-yet-accept()'d backlog entries are abandoned — a
 * real, disclosed simplification: nothing in this port tears down a
 * listening daemon socket while a connect() is mid-flight). Safe to call
 * with s == NULL. */
void unix_socket_unref(unix_socket_t *s);

/* SYS_BIND: registers path in the real fixed registry (this *is* the
 * registry — any in-use, listening socket's own ->path field is a live
 * entry, found by linear scan; mirrors fs/vfs.c's own small fixed
 * mount_table[] scan). -EADDRINUSE if already bound live elsewhere,
 * -EISCONN if this socket is already bound/connected. */
int unix_socket_bind(unix_socket_t *s, const char *path);
/* SYS_LISTEN: marks a bound socket as listening, ready for
 * unix_socket_connect() to find via path lookup. -EDESTADDRREQ if never
 * bound. backlog is clamped to the real fixed backlog array size, never
 * silently ignored beyond that. */
int unix_socket_listen(unix_socket_t *s, int backlog);
/* SYS_ACCEPT: blocks (wait_queue_sleep) until a real pending connect()
 * is queued, then returns a fresh, already-connected unix_socket_t for
 * the caller to install into their own new fd (mirrors SYS_PIPE
 * installing two fresh open_file_t's) — real POSIX accept() semantics.
 * -EINVAL if s isn't listening. */
unix_socket_t *unix_socket_accept(unix_socket_t *s);
/* SYS_CONNECT: real kernel-mediated rendezvous — see this header's own
 * top comment and syscall.h's SYS_CONNECT comment for why this completes
 * synchronously rather than blocking for the peer's accept() to happen
 * first (matching real AF_UNIX kernel behavior). -ECONNREFUSED if no
 * listening socket is registered at that path, -EISCONN if s is already
 * bound/connected. */
int unix_socket_connect(unix_socket_t *s, const char *path);

/* Real, blocking (wait_queue_sleep, same primitive as pipe_read()/
 * pipe_write() — see arch/i386/syscall.c's own copies of this exact
 * logic, duplicated here rather than shared since pipe_read()/
 * pipe_write() are static and keyed off open_file_t, not a bare
 * pipe_buf_t*) reads/writes against a *connected* socket's own rx/tx
 * rings. O_NONBLOCK is honored via the `nonblock` flag (the caller
 * already has it from f->flags, exactly like pipe_read()/pipe_write()).
 * -ENOTCONN if not yet connected. */
int unix_socket_read(unix_socket_t *s, char *buf, size_t len, bool nonblock);
int unix_socket_write(unix_socket_t *s, const char *buf, size_t len, bool nonblock);

#endif
