/* user/pude_qtclient.c — pude's app_class_t adapter (user/pude_app.h) for
 * an external Qt GUI client process (docs/qt-port.md Phase 6): a real Qt
 * Widgets/Gui application, statically linked against the "pureunix" QPA
 * plugin (user/qpa_pureunix/), fork/exec'd with two pipes wired up
 * exactly like PUTerm already wires a pty pair to its own forked shell
 * child (user/pude_term.c's spawn_shell()/puterm_create()) -- the design
 * docs/qt-port.md's Phase 1 audit (section 5) laid out: no changes to
 * pude's own core window-list/z-order/focus/render loop, this is just
 * another app_class_t.
 *
 * Protocol: user/pureunix_qpa_protocol.h (shared with the C++ QPA
 * plugin, plain C, extern "C") over two pipes dup2'd onto fixed fd
 * numbers in the child before execve() -- PUREUNIX_QPA_FD_READ (3, PUDE
 * -> client: input/resize/close) and PUREUNIX_QPA_FD_WRITE (4, client ->
 * PUDE: window create/damage/title/close).
 *
 * The client -> PUDE direction uses a real, non-blocking, incremental
 * message parser (poll()'d with a zero timeout every WM frame via the
 * real SYS_POLL primitive added in docs/qt-port.md's Phase 4 -- genuine
 * FD_KIND_PIPE readiness, not the old always-ready lie) rather than a
 * blocking read(): a single PU_QPA_C2S_DAMAGE message can be larger than
 * one pipe buffer's worth of bytes (a whole window's first paint,
 * easily hundreds of KB), and pude's own WM loop must never block
 * waiting for the rest of it to arrive -- an incomplete message just
 * waits in qtclient_state_t.rbuf for more data on a later frame, same
 * "never blocks, always makes progress" idiom PUTerm's own poll()
 * already uses for its pty master reads.
 *
 * The PUDE -> client direction (resize/close/keyboard/mouse/focus) is a
 * real, non-blocking, all-or-nothing write per message (see this file's
 * own send_message()) -- NOT a small blocking write like an earlier
 * version of this file used. A blocking write here can deadlock the
 * entire desktop: pude's single-threaded WM loop can be sending input
 * here at the exact moment the client is itself blocked mid-write()
 * sending a large PU_QPA_C2S_DAMAGE payload back over the *other* pipe
 * (real, reproduced bug -- see git history/docs/qt-port.md). Since
 * pude's own read of that C2S direction is always non-blocking/poll-
 * gated (previous paragraph), the fix only needed to make THIS
 * direction non-blocking too: once neither side can ever block on a
 * write, the deadlock cycle can't form, regardless of how large or
 * slow-draining the other side's payload is.
 */
#include "pude_qtclient.h"
#include "pude_gfx.h"
#include "pude_widgets.h"
#include "pureunix_qpa_protocol.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* See user/pude_qtclient.h's own comment. */
static char g_exec_path[256];
static bool g_has_exec_path;

void pude_qtclient_set_exec_path(const char *path)
{
    if (path && path[0]) {
        strncpy(g_exec_path, path, sizeof(g_exec_path) - 1);
        g_exec_path[sizeof(g_exec_path) - 1] = '\0';
        g_has_exec_path = true;
    } else {
        g_has_exec_path = false;
    }
}

/* Generous but bounded -- a real client sending an unreasonably large
 * single message (well beyond any real window's plausible full-repaint
 * size) gets its connection torn down rather than this process growing
 * its read buffer without limit. */
#define QTCLIENT_RBUF_MAX (8 * 1024 * 1024)

typedef struct {
    int read_fd;   /* PUDE reads client -> PUDE messages here */
    int write_fd;  /* PUDE writes PUDE -> client messages here */
    pid_t child;
    bool child_alive;
    bool protocol_error; /* malformed/oversized message -- treat like a dead child */

    uint32_t *content;   /* content_w * content_h pixels, ARGB32 (native
                           * byte order == QImage::Format_ARGB32's own
                           * in-memory layout on this little-endian
                           * target -- see pureunixbackingstore.cpp's own
                           * comment), the client's most recent full
                           * window image */
    int content_w, content_h;
    bool dirty;

    char title[48];
    bool title_changed;

    uint8_t *rbuf;
    size_t rbuf_len;
    size_t rbuf_cap;

    /* PU_QPA_C2S_RESIZE_REQUEST (user/pureunix_qpa_protocol.h's own
     * comment has the full story): Qt's own layout can decide it needs a
     * client area bigger than what this window was created with. Applied
     * once per WM frame in qtclient_poll() (which alone has the
     * pude_window_t* needed to actually resize the window's own chrome),
     * not directly in handle_message() -- same "stash it, apply it where
     * the right context exists" split resize_content() itself doesn't
     * need since it only ever touches this struct's own fields. */
    bool has_pending_resize;
    int pending_resize_w, pending_resize_h;
} qtclient_state_t;

static bool rbuf_ensure(qtclient_state_t *st, size_t need)
{
    if (st->rbuf_cap >= need) {
        return true;
    }
    size_t new_cap = st->rbuf_cap ? st->rbuf_cap : 4096;
    while (new_cap < need) {
        new_cap *= 2;
    }
    if (new_cap > QTCLIENT_RBUF_MAX) {
        return false;
    }
    uint8_t *n = realloc(st->rbuf, new_cap);
    if (!n) {
        return false;
    }
    st->rbuf = n;
    st->rbuf_cap = new_cap;
    return true;
}

static void resize_content(qtclient_state_t *st, int w, int h)
{
    if (w == st->content_w && h == st->content_h) {
        return;
    }
    uint32_t *n = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!n) {
        return; /* keep the old buffer; a later damage message will retry */
    }
    free(st->content);
    st->content = n;
    st->content_w = w;
    st->content_h = h;
    st->dirty = true;
}

/* Applies one already-fully-buffered PU_QPA_C2S_* message. */
static void handle_message(qtclient_state_t *st, uint32_t type, const uint8_t *payload, uint32_t len)
{
    switch (type) {
    case PU_QPA_C2S_WINDOW_CREATE: {
        if (len < sizeof(pu_qpa_window_create_t)) {
            break;
        }
        pu_qpa_window_create_t c;
        memcpy(&c, payload, sizeof(c));
        if (c.default_w > 0 && c.default_h > 0 && c.default_w <= 4096 && c.default_h <= 4096) {
            resize_content(st, c.default_w, c.default_h);
        }
        break;
    }
    case PU_QPA_C2S_DAMAGE: {
        if (len < sizeof(pu_qpa_damage_t)) {
            break;
        }
        pu_qpa_damage_t d;
        memcpy(&d, payload, sizeof(d));
        const uint8_t *pixels = payload + sizeof(d);
        size_t pixel_bytes = len - sizeof(d);
        if (d.w <= 0 || d.h <= 0 || (size_t)d.w * (size_t)d.h * 4 != pixel_bytes) {
            break; /* malformed -- silently drop this one message, connection stays up */
        }
        if (!st->content || d.x < 0 || d.y < 0 ||
            d.x + d.w > st->content_w || d.y + d.h > st->content_h) {
            break; /* damage rect outside the last known window size -- stale, drop it */
        }
        for (int row = 0; row < d.h; row++) {
            memcpy(&st->content[(size_t)(d.y + row) * st->content_w + d.x],
                   pixels + (size_t)row * d.w * 4, (size_t)d.w * 4);
        }
        st->dirty = true;
        break;
    }
    case PU_QPA_C2S_RESIZE_REQUEST: {
        if (len < sizeof(pu_qpa_size_t)) {
            break;
        }
        pu_qpa_size_t sz;
        memcpy(&sz, payload, sizeof(sz));
        if (sz.w > 0 && sz.h > 0 && sz.w <= 4096 && sz.h <= 4096) {
            st->pending_resize_w = sz.w;
            st->pending_resize_h = sz.h;
            st->has_pending_resize = true;
        }
        break;
    }
    case PU_QPA_C2S_SET_TITLE: {
        size_t n = len < sizeof(st->title) - 1 ? len : sizeof(st->title) - 1;
        memcpy(st->title, payload, n);
        st->title[n] = '\0';
        st->title_changed = true;
        break;
    }
    case PU_QPA_C2S_CLOSE:
        st->child_alive = false;
        break;
    default:
        break; /* unknown message type -- forward-compatible no-op */
    }
}

/* Drains as many complete (header + payload) messages as are currently
 * buffered, leaving any trailing partial message in place for next
 * time -- see this file's own header comment for why a real client ->
 * PUDE message can legitimately arrive split across several poll()
 * calls. */
static void drain_messages(qtclient_state_t *st)
{
    size_t off = 0;
    for (;;) {
        if (st->rbuf_len - off < sizeof(pu_qpa_msg_header_t)) {
            break;
        }
        pu_qpa_msg_header_t hdr;
        memcpy(&hdr, st->rbuf + off, sizeof(hdr));
        size_t total = sizeof(hdr) + hdr.len;
        if (total > QTCLIENT_RBUF_MAX) {
            st->protocol_error = true;
            off = st->rbuf_len;
            break;
        }
        if (st->rbuf_len - off < total) {
            break; /* payload not fully arrived yet */
        }
        handle_message(st, hdr.type, st->rbuf + off + sizeof(hdr), hdr.len);
        off += total;
    }
    if (off > 0) {
        memmove(st->rbuf, st->rbuf + off, st->rbuf_len - off);
        st->rbuf_len -= off;
    }
}

/* Every S2C_* payload struct in user/pureunix_qpa_protocol.h is well
 * under this -- generous headroom for a header + the largest one
 * (pu_qpa_mouse_button_t) without needing to size this per-caller. */
#define QTCLIENT_S2C_MSG_MAX 64

static void send_message(qtclient_state_t *st, uint32_t type, const void *payload, uint32_t len)
{
    /* Real, non-blocking, all-or-nothing send -- NOT the small-blocking-
     * write "accepted simplification" this function used to be (see
     * git history/docs/qt-port.md for the full story): that design
     * caused a genuine bidirectional-pipe deadlock. `pude`'s own single-
     * threaded WM loop calls this (forwarding keyboard/mouse input) while
     * the very same Qt client can itself be blocked mid-write() sending
     * pude a large PU_QPA_C2S_DAMAGE repaint over the *other* pipe --
     * a blocking write() here could freeze pude's entire WM loop
     * forever, which in turn could never get back around to draining the
     * client's own pending damage either. Every message here is tiny
     * (see QTCLIENT_S2C_MSG_MAX) relative to the real 4096-byte pipe
     * buffer, so this fits in one combined, single write() call whenever
     * the client is keeping up at all -- if it doesn't fit right now
     * (client genuinely backed up), the whole message is dropped rather
     * than partially written, since a partial header+payload would
     * desync every future message's framing forever. Dropping an
     * occasional input event under real backpressure (the client would
     * have to be badly behind) is far preferable to a hung desktop. */
    if (len > QTCLIENT_S2C_MSG_MAX - sizeof(pu_qpa_msg_header_t)) {
        return; /* not reachable by any real caller today -- see the enum above */
    }
    uint8_t buf[QTCLIENT_S2C_MSG_MAX];
    pu_qpa_msg_header_t hdr;
    hdr.type = type;
    hdr.len = len;
    memcpy(buf, &hdr, sizeof(hdr));
    if (len > 0) {
        memcpy(buf + sizeof(hdr), payload, len);
    }
    size_t total = sizeof(hdr) + len;
    fcntl(st->write_fd, F_SETFL, O_NONBLOCK);
    ssize_t n = write(st->write_fd, buf, total);
    if (n < 0 || (size_t)n != total) {
        return; /* dropped -- client's own input pipe is currently full */
    }
}

static pid_t spawn_client(const char *path, int child_read_fd, int child_write_fd)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        /* A job-control shell (BusyBox ash, what PUTerm's own identical-
         * looking spawn_shell() forks) calls setpgid(0,0) on itself at
         * startup, which is what actually makes that file's own
         * `kill(-st->child, SIGHUP)` reach it -- an ordinary Qt binary has
         * no reason to know about that convention and never does, so
         * without this call here it silently inherits pude's own pgid
         * instead of getting one matching its own pid. kill(-st->child,
         * ...) then targets a process group nothing is actually in, a
         * real no-op that was found the hard way: pude's own Ctrl+F12
         * "emergency whole-desktop quit" hung forever inside qtclient_
         * destroy()'s waitpid() because the signal it sent first never
         * reached this process at all. Set from both sides (the classic
         * fork/exec race guard) -- see qtclient_create() below for the
         * parent-side call. */
        setpgid(0, 0);
        if (child_read_fd != PUREUNIX_QPA_FD_READ) {
            dup2(child_read_fd, PUREUNIX_QPA_FD_READ);
            close(child_read_fd);
        }
        if (child_write_fd != PUREUNIX_QPA_FD_WRITE) {
            dup2(child_write_fd, PUREUNIX_QPA_FD_WRITE);
            close(child_write_fd);
        }
        char *argv[] = { (char *)path, NULL };
        extern char **environ;
        execve(path, argv, environ);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

static void *qtclient_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    if (!g_has_exec_path) {
        return NULL; /* no program named -- see pude_qtclient_set_exec_path() */
    }

    qtclient_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }

    int to_client[2];  /* PUDE -> client: write end stays with PUDE, read end goes to the child */
    int from_client[2]; /* client -> PUDE: read end stays with PUDE, write end goes to the child */
    if (pipe(to_client) != 0) {
        free(st);
        return NULL;
    }
    if (pipe(from_client) != 0) {
        close(to_client[0]);
        close(to_client[1]);
        free(st);
        return NULL;
    }

    st->child = spawn_client(g_exec_path, to_client[0], from_client[1]);
    if (st->child < 0) {
        close(to_client[0]); close(to_client[1]);
        close(from_client[0]); close(from_client[1]);
        free(st);
        return NULL;
    }
    /* PUDE only needs the write end of to_client and the read end of
     * from_client -- the child inherited what it needs (or duplicates
     * of them) already, exactly like PUTerm closes its own copy of
     * slave_fd after forking. */
    close(to_client[0]);
    close(from_client[1]);
    st->write_fd = to_client[1];
    st->read_fd = from_client[0];
    st->child_alive = true;

    resize_content(st, client_w, client_h);
    strncpy(st->title, win->title, sizeof(st->title) - 1);
    return st;
}

static void qtclient_destroy(pude_window_t *win, void *state)
{
    (void)win;
    qtclient_state_t *st = state;
    if (st->child_alive) {
        /* Unlike a plain shell child (user/pude_term.c's own identical-
         * looking SIGHUP+blocking-waitpid pair, which real shells always
         * terminate on), a real QGuiApplication event loop is not
         * guaranteed to die from SIGHUP alone -- blocking here forever
         * would freeze pude's entire single-threaded WM loop (found via
         * this exact hang: Ctrl+F12's "emergency whole-desktop quit" never
         * returned to the outer shell while a Qt child was still up).
         * Give it a bounded grace period via non-blocking polls, then
         * escalate to SIGKILL -- still correct for the common case (an
         * app that only takes one event-loop iteration to unwind after
         * SIGHUP) without ever risking an unbounded wait. */
        kill(-st->child, SIGHUP);
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 50; i++) { /* ~500ms at 10ms/iteration */
            pid_t r = waitpid(st->child, &status, WNOHANG);
            if (r == st->child) {
                exited = true;
                break;
            }
            usleep(10000);
        }
        if (!exited) {
            kill(-st->child, SIGKILL);
            waitpid(st->child, &status, 0);
        }
    }
    close(st->write_fd);
    close(st->read_fd);
    free(st->content);
    free(st->rbuf);
    free(st);
}

static void qtclient_render(pude_window_t *win, void *state, SDL_Surface *s,
                            int cx, int cy, int cw, int ch)
{
    (void)win;
    qtclient_state_t *st = state;
    if (!st->content) {
        pu_fill_rect(s, cx, cy, cw, ch, SDL_MapRGB(s->format, 30, 30, 30));
        return;
    }
    int copy_w = st->content_w < cw ? st->content_w : cw;
    int copy_h = st->content_h < ch ? st->content_h : ch;
    for (int row = 0; row < copy_h; row++) {
        Uint8 *dst_row = (Uint8 *)s->pixels + (size_t)(cy + row) * s->pitch + (size_t)cx * 4;
        memcpy(dst_row, &st->content[(size_t)row * st->content_w], (size_t)copy_w * 4);
    }
    /* Any area the client's own image doesn't yet cover (window just
     * grew, or the client hasn't painted this row range yet) reads as
     * plain black rather than stale/garbage bytes. */
    if (copy_w < cw) {
        pu_fill_rect(s, cx + copy_w, cy, cw - copy_w, ch, SDL_MapRGB(s->format, 0, 0, 0));
    }
    if (copy_h < ch) {
        pu_fill_rect(s, cx, cy + copy_h, cw, ch - copy_h, SDL_MapRGB(s->format, 0, 0, 0));
    }
}

static void qtclient_on_key(pude_window_t *win, void *state, SDL_Scancode sc,
                            key_mods_t mods, bool down)
{
    (void)win;
    qtclient_state_t *st = state;
    if (!st->child_alive) {
        return;
    }
    pu_qpa_key_t k;
    memset(&k, 0, sizeof(k));
    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: k.key_code = PU_QPA_KEY_RETURN; break;
    case SDL_SCANCODE_BACKSPACE: k.key_code = PU_QPA_KEY_BACKSPACE; break;
    case SDL_SCANCODE_TAB: k.key_code = PU_QPA_KEY_TAB; break;
    case SDL_SCANCODE_ESCAPE: k.key_code = PU_QPA_KEY_ESCAPE; break;
    case SDL_SCANCODE_UP: k.key_code = PU_QPA_KEY_UP; break;
    case SDL_SCANCODE_DOWN: k.key_code = PU_QPA_KEY_DOWN; break;
    case SDL_SCANCODE_LEFT: k.key_code = PU_QPA_KEY_LEFT; break;
    case SDL_SCANCODE_RIGHT: k.key_code = PU_QPA_KEY_RIGHT; break;
    case SDL_SCANCODE_HOME: k.key_code = PU_QPA_KEY_HOME; break;
    case SDL_SCANCODE_END: k.key_code = PU_QPA_KEY_END; break;
    case SDL_SCANCODE_PAGEUP: k.key_code = PU_QPA_KEY_PAGEUP; break;
    case SDL_SCANCODE_PAGEDOWN: k.key_code = PU_QPA_KEY_PAGEDOWN; break;
    case SDL_SCANCODE_DELETE: k.key_code = PU_QPA_KEY_DELETE; break;
    case SDL_SCANCODE_INSERT: k.key_code = PU_QPA_KEY_INSERT; break;
    case SDL_SCANCODE_F1: k.key_code = PU_QPA_KEY_F1; break;
    case SDL_SCANCODE_F2: k.key_code = PU_QPA_KEY_F2; break;
    case SDL_SCANCODE_F3: k.key_code = PU_QPA_KEY_F3; break;
    case SDL_SCANCODE_F4: k.key_code = PU_QPA_KEY_F4; break;
    case SDL_SCANCODE_F5: k.key_code = PU_QPA_KEY_F5; break;
    case SDL_SCANCODE_F6: k.key_code = PU_QPA_KEY_F6; break;
    case SDL_SCANCODE_F7: k.key_code = PU_QPA_KEY_F7; break;
    case SDL_SCANCODE_F8: k.key_code = PU_QPA_KEY_F8; break;
    case SDL_SCANCODE_F9: k.key_code = PU_QPA_KEY_F9; break;
    case SDL_SCANCODE_F10: k.key_code = PU_QPA_KEY_F10; break;
    case SDL_SCANCODE_F11: k.key_code = PU_QPA_KEY_F11; break;
    case SDL_SCANCODE_F12: k.key_code = PU_QPA_KEY_F12; break;
    default: k.key_code = PU_QPA_KEY_NONE; break;
    }
    k.shift = mods.shift;
    k.ctrl = mods.ctrl;
    k.down = down;
    k.ascii_char = (uint8_t)pu_scancode_to_ascii(sc, mods);
    send_message(st, PU_QPA_S2C_KEY, &k, sizeof(k));
}

static void qtclient_on_mouse_down(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    qtclient_state_t *st = state;
    if (!st->child_alive) return;
    pu_qpa_mouse_button_t m = { .x = x, .y = y, .button = 0, .down = 1 };
    send_message(st, PU_QPA_S2C_MOUSE_BUTTON, &m, sizeof(m));
}

static void qtclient_on_mouse_up(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    qtclient_state_t *st = state;
    if (!st->child_alive) return;
    pu_qpa_mouse_button_t m = { .x = x, .y = y, .button = 0, .down = 0 };
    send_message(st, PU_QPA_S2C_MOUSE_BUTTON, &m, sizeof(m));
}

static void qtclient_on_mouse_move(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    qtclient_state_t *st = state;
    if (!st->child_alive) return;
    pu_qpa_point_t p = { .x = x, .y = y };
    send_message(st, PU_QPA_S2C_MOUSE_MOVE, &p, sizeof(p));
}

static void qtclient_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    qtclient_state_t *st = state;
    resize_content(st, new_client_w, new_client_h);
    if (!st->child_alive) return;
    pu_qpa_size_t sz = { .w = new_client_w, .h = new_client_h };
    send_message(st, PU_QPA_S2C_RESIZE, &sz, sizeof(sz));
}

static bool qtclient_poll(pude_window_t *win, void *state)
{
    qtclient_state_t *st = state;

    if (st->child_alive) {
        int status = 0;
        pid_t r = waitpid(st->child, &status, WNOHANG);
        if (r == st->child) {
            st->child_alive = false;
        }
    }

    /* Real, non-blocking readiness check (SYS_POLL, docs/qt-port.md
     * Phase 4) before ever calling read() -- this function runs once per
     * WM frame for every open window, so it must never block. */
    struct pollfd pfd = { .fd = st->read_fd, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        uint8_t chunk[4096];
        int n = (int)read(st->read_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        if (!rbuf_ensure(st, st->rbuf_len + (size_t)n)) {
            st->protocol_error = true;
            break;
        }
        memcpy(st->rbuf + st->rbuf_len, chunk, (size_t)n);
        st->rbuf_len += (size_t)n;
        drain_messages(st);
        pfd.revents = 0;
    }

    if (st->protocol_error) {
        st->child_alive = false;
    }

    /* Applies a pending PU_QPA_C2S_RESIZE_REQUEST (see that enum's own
     * comment) -- this is the one place in this file that actually has
     * both the pude_window_t* (whole-window geometry, WM-owned) and the
     * client's own idea of its new client-area size, so it's the only
     * place this can happen. win->w/win->h already equal client_w/h plus
     * a constant chrome amount (border + title bar, both private to
     * user/pude.c) -- rather than duplicating those constants here, this
     * just applies the *delta* between the old and new client size,
     * which maps 1:1 onto the same delta in whole-window size regardless
     * of what the chrome thickness actually is. Exactly the same
     * pude_window_t.w/h fields a user's own drag-resize already mutates
     * (user/pude.c) -- no new WM concept, just a second way to drive it. */
    if (st->has_pending_resize) {
        st->has_pending_resize = false;
        int new_w = st->pending_resize_w;
        int new_h = st->pending_resize_h;
        if (new_w < win->cls->min_client_w) new_w = win->cls->min_client_w;
        if (new_h < win->cls->min_client_h) new_h = win->cls->min_client_h;
        if (new_w != st->content_w || new_h != st->content_h) {
            win->w += new_w - st->content_w;
            win->h += new_h - st->content_h;
            resize_content(st, new_w, new_h);
        }
    }

    if (win->title[0] == '\0' || st->title_changed) {
        if (st->title_changed) {
            strncpy(win->title, st->title, sizeof(win->title) - 1);
            st->title_changed = false;
        }
    }

    if (st->dirty) {
        st->dirty = false;
        return true;
    }
    return false;
}

static bool qtclient_is_alive(pude_window_t *win, void *state)
{
    (void)win;
    qtclient_state_t *st = state;
    return st->child_alive;
}

const app_class_t qtclient_app_class = {
    .name = "Qt Application",
    .default_client_w = 400,
    .default_client_h = 300,
    .min_client_w = 100,
    .min_client_h = 80,
    .create = qtclient_create,
    .destroy = qtclient_destroy,
    .render = qtclient_render,
    .on_key = qtclient_on_key,
    .on_mouse_down = qtclient_on_mouse_down,
    .on_mouse_up = qtclient_on_mouse_up,
    .on_mouse_move = qtclient_on_mouse_move,
    .on_resize = qtclient_on_resize,
    .poll = qtclient_poll,
    .is_alive = qtclient_is_alive,
    .icon_draw = NULL,
    .graphical = true,
    .pinned_default = false,
};

/* See this struct's own comment in user/pude_qtclient.h. */
const app_class_t qtclient_widgets_app_class = {
    .name = "Qt Widgets Test",
    .default_client_w = 420,
    .default_client_h = 320,
    .min_client_w = 200,
    .min_client_h = 150,
    .create = qtclient_create,
    .destroy = qtclient_destroy,
    .render = qtclient_render,
    .on_key = qtclient_on_key,
    .on_mouse_down = qtclient_on_mouse_down,
    .on_mouse_up = qtclient_on_mouse_up,
    .on_mouse_move = qtclient_on_mouse_move,
    .on_resize = qtclient_on_resize,
    .poll = qtclient_poll,
    .is_alive = qtclient_is_alive,
    .icon_draw = NULL,
    .graphical = true,
    .pinned_default = false,
};
