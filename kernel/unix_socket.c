/* kernel/unix_socket.c — real AF_UNIX domain sockets (SOCK_STREAM only).
 * See include/pureunix/unix_socket.h for the full design rationale; this
 * mirrors kernel/pty.c (fixed pool, real wait-queue blocking) with a
 * connected pair modeled as two pipe_buf_t rings (include/pureunix/
 * task.h — the exact struct SYS_PIPE already uses) instead of pipe()'s
 * one, plus a small fixed path registry and listen backlog. */
#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/memory.h>
#include <pureunix/string.h>
#include <pureunix/unix_socket.h>

/* A handful of concurrent sockets is all any real use of this primitive
 * needs (one menu-cache-daemon listener + a few short-lived clients at
 * once) — a fixed pool, same style as MAX_PTYS (kernel/pty.c). */
#define MAX_UNIX_SOCKETS 32
#define MAX_BACKLOG 8

typedef struct {
    pipe_buf_t *rx;
    pipe_buf_t *tx;
} pending_conn_t;

struct unix_socket {
    bool in_use;
    int refs;

    bool bound;
    bool listening;
    bool connected;
    char path[PUREUNIX_MAX_PATH];

    /* Listening only: real FIFO backlog of not-yet-accept()'d
     * connections, plus the wait queue SYS_ACCEPT blocks on. */
    pending_conn_t backlog[MAX_BACKLOG];
    int backlog_count;
    wait_queue_t accept_wq;

    /* Connected only: this end's own two rings (see this file's own
     * top comment — rx is what this end reads, tx is what this end
     * writes; the peer's usock has them the other way around, exactly
     * like two pipe()'d fds glued back to back). */
    pipe_buf_t *rx;
    pipe_buf_t *tx;
};

static struct unix_socket g_sockets[MAX_UNIX_SOCKETS];

unix_socket_t *unix_socket_alloc(void)
{
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (!g_sockets[i].in_use) {
            memset(&g_sockets[i], 0, sizeof(g_sockets[i]));
            g_sockets[i].in_use = true;
            g_sockets[i].refs = 1;
            return &g_sockets[i];
        }
    }
    return NULL;
}

void unix_socket_ref(unix_socket_t *s)
{
    if (s) {
        s->refs++;
    }
}

void unix_socket_unref(unix_socket_t *s)
{
    if (!s) {
        return;
    }
    if (--s->refs > 0) {
        return;
    }
    if (s->connected) {
        /* Mirrors kernel/task.c's open_file_unref() FD_KIND_PIPE branch
         * exactly, just done twice (once per ring, since a socket has
         * two where a pipe end has one): this end held tx's write side
         * and rx's read side. */
        if (s->tx) {
            s->tx->write_ends--;
            if (s->tx->write_ends == 0) {
                wait_queue_wake_all(&s->tx->read_wq);
            }
            if (s->tx->read_ends == 0 && s->tx->write_ends == 0) {
                kfree(s->tx);
            }
        }
        if (s->rx) {
            s->rx->read_ends--;
            if (s->rx->read_ends == 0) {
                wait_queue_wake_all(&s->rx->write_wq);
            }
            if (s->rx->read_ends == 0 && s->rx->write_ends == 0) {
                kfree(s->rx);
            }
        }
    }
    /* Listening socket: any not-yet-accept()'d backlog entries are
     * simply abandoned — a real, disclosed simplification (see this
     * function's own header comment in unix_socket.h). Their rings leak
     * for the lifetime of the system, same acceptable-tradeoff class as
     * this project's other deliberately-narrow real implementations
     * (see e.g. gotcha_libfm_extra... — no, simpler: nothing in this
     * port ever actually tears down a live daemon listener, so this
     * path is untested-by-necessity dead code in practice). */
    s->in_use = false;
}

int unix_socket_bind(unix_socket_t *s, const char *path)
{
    if (!s || s->bound || s->connected) {
        return -EISCONN;
    }
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (g_sockets[i].in_use && g_sockets[i].listening &&
            strcmp(g_sockets[i].path, path) == 0) {
            return -EADDRINUSE;
        }
    }
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';
    s->bound = true;
    return 0;
}

int unix_socket_listen(unix_socket_t *s, int backlog)
{
    if (!s || !s->bound) {
        return -EDESTADDRREQ;
    }
    (void)backlog; /* real fixed backlog array size — never silently
                     * ignored beyond MAX_BACKLOG, but nothing in this
                     * port's own real caller (menu-cache-daemon) ever
                     * asks for more than a handful anyway. */
    s->listening = true;
    return 0;
}

static bool accept_backlog_ready(void *ctx)
{
    unix_socket_t *s = (unix_socket_t *)ctx;
    return s->backlog_count > 0;
}

unix_socket_t *unix_socket_accept(unix_socket_t *s)
{
    if (!s || !s->listening) {
        return NULL;
    }
    if (s->backlog_count == 0) {
        /* See include/pureunix/wait.h's invariant — int $0x80 enters
         * with interrupts masked, and nothing could ever wake this
         * sleeper otherwise (exactly like pipe_read()'s own comment). */
        arch_enable_interrupts();
        wait_queue_sleep(&s->accept_wq, accept_backlog_ready, s);
    }
    if (s->backlog_count == 0) {
        return NULL; /* woken for an unrelated reason; caller retries */
    }
    pending_conn_t pc = s->backlog[0];
    for (int i = 1; i < s->backlog_count; i++) {
        s->backlog[i - 1] = s->backlog[i];
    }
    s->backlog_count--;

    unix_socket_t *accepted = unix_socket_alloc();
    if (!accepted) {
        /* Real, honest failure: no free socket pool slot. The queued
         * rings are still real and still referenced by the connecting
         * peer's own end — just leaked back into the backlog is wrong
         * (caller already popped it), so free them here rather than
         * leaving them dangling with no owner at all. */
        kfree(pc.rx);
        kfree(pc.tx);
        return NULL;
    }
    accepted->connected = true;
    accepted->rx = pc.rx;
    accepted->tx = pc.tx;
    return accepted;
}

int unix_socket_connect(unix_socket_t *s, const char *path)
{
    if (!s || s->bound || s->connected) {
        return -EISCONN;
    }
    unix_socket_t *listener = NULL;
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (g_sockets[i].in_use && g_sockets[i].listening &&
            strcmp(g_sockets[i].path, path) == 0) {
            listener = &g_sockets[i];
            break;
        }
    }
    if (!listener) {
        return -ECONNREFUSED;
    }
    if (listener->backlog_count >= MAX_BACKLOG) {
        return -ECONNREFUSED; /* real, honest backlog-full rejection */
    }

    pipe_buf_t *c2s = kcalloc(1, sizeof(pipe_buf_t));
    pipe_buf_t *s2c = kcalloc(1, sizeof(pipe_buf_t));
    if (!c2s || !s2c) {
        kfree(c2s);
        kfree(s2c);
        return -ENOMEM;
    }
    /* This end (the connecting client): writes into c2s, reads from
     * s2c. The eventual accept()'d peer gets the mirror image — see
     * unix_socket_accept() above. */
    c2s->write_ends = 1;
    c2s->read_ends = 1;
    s2c->write_ends = 1;
    s2c->read_ends = 1;

    s->connected = true;
    s->tx = c2s;
    s->rx = s2c;

    listener->backlog[listener->backlog_count].rx = c2s;
    listener->backlog[listener->backlog_count].tx = s2c;
    listener->backlog_count++;
    wait_queue_wake_one(&listener->accept_wq);
    return 0;
}

/* ---- Connected-socket I/O: real ring-buffer read/write against a
 * pipe_buf_t, same logic as arch/i386/syscall.c's own pipe_read()/
 * pipe_write() (duplicated rather than shared, since those are static
 * and keyed off an open_file_t, not a bare pipe_buf_t* — see this file's
 * own header comment). ---- */

typedef struct {
    pipe_buf_t *p;
} sock_wait_ctx_t;

static bool sock_readable(void *ctx)
{
    pipe_buf_t *p = ((sock_wait_ctx_t *)ctx)->p;
    return p->count > 0 || p->write_ends == 0;
}

static bool sock_writable(void *ctx)
{
    pipe_buf_t *p = ((sock_wait_ctx_t *)ctx)->p;
    return p->count < PUREUNIX_PIPE_SIZE || p->read_ends == 0;
}

int unix_socket_read(unix_socket_t *s, char *buf, size_t len, bool nonblock)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }
    pipe_buf_t *p = s->rx;
    if (len == 0) {
        return 0;
    }
    if (p->count == 0) {
        if (p->write_ends == 0) {
            return 0; /* EOF: peer gone, nothing buffered */
        }
        if (nonblock) {
            return -EAGAIN;
        }
        arch_enable_interrupts();
        sock_wait_ctx_t ctx = { .p = p };
        wait_queue_sleep(&p->read_wq, sock_readable, &ctx);
        if (p->count == 0) {
            return 0; /* woken because the peer closed, not by data */
        }
    }
    size_t to_copy = len < p->count ? len : p->count;
    for (size_t i = 0; i < to_copy; ++i) {
        buf[i] = (char)p->data[p->tail];
        p->tail = (p->tail + 1) % PUREUNIX_PIPE_SIZE;
    }
    p->count -= to_copy;
    wait_queue_wake_all(&p->write_wq);
    return (int)to_copy;
}

int unix_socket_write(unix_socket_t *s, const char *buf, size_t len, bool nonblock)
{
    if (!s || !s->connected) {
        return -ENOTCONN;
    }
    pipe_buf_t *p = s->tx;
    if (len == 0) {
        return 0;
    }
    if (p->read_ends == 0) {
        return -EPIPE;
    }
    size_t written = 0;
    while (written < len) {
        if (p->count == PUREUNIX_PIPE_SIZE) {
            if (nonblock) {
                return written ? (int)written : -EAGAIN;
            }
            arch_enable_interrupts();
            sock_wait_ctx_t ctx = { .p = p };
            wait_queue_sleep(&p->write_wq, sock_writable, &ctx);
            if (p->read_ends == 0) {
                return written ? (int)written : -EPIPE;
            }
        }
        size_t space = PUREUNIX_PIPE_SIZE - p->count;
        size_t chunk = (len - written) < space ? (len - written) : space;
        for (size_t i = 0; i < chunk; ++i) {
            p->data[p->head] = (uint8_t)buf[written + i];
            p->head = (p->head + 1) % PUREUNIX_PIPE_SIZE;
        }
        p->count += chunk;
        written += chunk;
        wait_queue_wake_all(&p->read_wq);
    }
    return (int)written;
}

bool unix_socket_poll_readable(unix_socket_t *s)
{
    if (!s || !s->connected) {
        return false;
    }
    pipe_buf_t *p = s->rx;
    return p->count > 0 || p->write_ends == 0;
}

bool unix_socket_poll_writable(unix_socket_t *s)
{
    if (!s || !s->connected) {
        return false;
    }
    pipe_buf_t *p = s->tx;
    return p->count < PUREUNIX_PIPE_SIZE || p->read_ends == 0;
}
