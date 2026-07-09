#include <pureunix/editor.h>
#include <pureunix/keyboard.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

#define VIM_MAX_LINES 512
#define VIM_MAX_COL   256

/* Content rows/cols, recomputed from the real console size (vga_get_size())
 * each time editor_open() runs — the last two screen rows are always the
 * status bar and command line, so content gets whatever's left. Falls back
 * to the historical 23x80 if the console ever reports something smaller
 * than that (shouldn't happen: legacy VGA text mode is always >= 25x80). */
static size_t vim_rows = 23;
static size_t vim_cols = 80;

typedef enum { NORMAL, INSERT, COMMAND, VSEARCH } vim_mode_t;

typedef struct {
    char       path[128];
    char      *buf[VIM_MAX_LINES];
    size_t     nlines;
    size_t     cx, cy;
    size_t     row_off, col_off;
    bool       dirty;
    bool       running;
    vim_mode_t mode;
    int        pending;          /* partial normal cmds: 'd','g','y','c','r' */
    char       cmd[80];
    size_t     cmdlen;
    char       yank[VIM_MAX_COL];
    bool       has_yank;
    char      *ubuf[VIM_MAX_LINES];
    size_t     unlines;
    size_t     ucx, ucy;
    bool       has_undo;
    char       search[64];
    size_t     search_len;
    char       msg[80];
} vim_t;

static vim_t V;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static char *lalloc(const char *src, size_t len)
{
    char *p = kmalloc(VIM_MAX_COL);
    if (!p) return NULL;
    memset(p, 0, VIM_MAX_COL);
    if (src && len) {
        if (len >= VIM_MAX_COL) len = VIM_MAX_COL - 1;
        memcpy(p, src, len);
    }
    return p;
}

static void set_msg(const char *s)
{
    strncpy(V.msg, s, sizeof(V.msg) - 1);
    V.msg[sizeof(V.msg) - 1] = '\0';
}

static void clamp_cx(void)
{
    size_t len = strlen(V.buf[V.cy]);
    size_t max = (V.mode == INSERT) ? len : (len > 0 ? len - 1 : 0);
    if (V.cx > max) V.cx = max;
}

static void do_scroll(void)
{
    if (V.cy < V.row_off)              V.row_off = V.cy;
    if (V.cy >= V.row_off + vim_rows)  V.row_off = V.cy - vim_rows + 1;
    if (V.cx < V.col_off)              V.col_off = V.cx;
    if (V.cx >= V.col_off + vim_cols)  V.col_off = V.cx - vim_cols + 1;
}

/* ── undo ─────────────────────────────────────────────────────────────────── */

static void save_undo(void)
{
    if (V.has_undo)
        for (size_t i = 0; i < V.unlines; ++i) kfree(V.ubuf[i]);
    V.unlines = V.nlines;
    V.ucx = V.cx;
    V.ucy = V.cy;
    for (size_t i = 0; i < V.nlines; ++i)
        V.ubuf[i] = lalloc(V.buf[i], strlen(V.buf[i]));
    V.has_undo = true;
}

static void do_undo(void)
{
    if (!V.has_undo) { set_msg("Already at oldest change"); return; }
    for (size_t i = 0; i < V.nlines; ++i) kfree(V.buf[i]);
    V.nlines = V.unlines;
    V.cx = V.ucx;
    V.cy = V.ucy;
    for (size_t i = 0; i < V.nlines; ++i) { V.buf[i] = V.ubuf[i]; V.ubuf[i] = NULL; }
    V.has_undo = false;
    V.dirty = true;
    set_msg("1 change; before #1");
}

/* ── file I/O ─────────────────────────────────────────────────────────────── */

static void buf_free(void)
{
    for (size_t i = 0; i < V.nlines; ++i) { kfree(V.buf[i]); V.buf[i] = NULL; }
    V.nlines = 0;
}

static void load_file(const char *path)
{
    buf_free();
    strncpy(V.path, path, sizeof(V.path) - 1);
    uint8_t *data = NULL;
    size_t   size = 0;
    if (vfs_read_file(path, &data, &size) != 0) {
        V.buf[0] = lalloc("", 0);
        V.nlines = 1;
        set_msg("[New File]");
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i <= size && V.nlines < VIM_MAX_LINES; ++i) {
        if (i == size || data[i] == '\n') {
            char *ln = lalloc((char *)data + start, i - start);
            if (!ln) break;
            V.buf[V.nlines++] = ln;
            start = i + 1;
        }
    }
    if (V.nlines > 1 && size > 0 && data[size - 1] == '\n' &&
        strlen(V.buf[V.nlines - 1]) == 0) {
        kfree(V.buf[V.nlines - 1]);
        V.buf[V.nlines - 1] = NULL;
        V.nlines--;
    }
    if (V.nlines == 0) { V.buf[0] = lalloc("", 0); V.nlines = 1; }
    kfree(data);
    snprintf(V.msg, sizeof(V.msg), "\"%s\" %uL", path, (uint32_t)V.nlines);
}

static int save_file(void)
{
    size_t total = 0;
    for (size_t i = 0; i < V.nlines; ++i) total += strlen(V.buf[i]) + 1;
    char *out = kmalloc(total + 1);
    if (!out) { set_msg("Write failed: OOM"); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < V.nlines; ++i) {
        size_t len = strlen(V.buf[i]);
        memcpy(out + pos, V.buf[i], len);
        pos += len;
        out[pos++] = '\n';
    }
    if (!vfs_mounted()) {
        kfree(out);
        set_msg("Write error: no filesystem");
        return -1;
    }
    int r = vfs_write_file(V.path, (const uint8_t *)out, pos, VFS_O_TRUNC);
    kfree(out);
    if (r == 0) {
        V.dirty = false;
        snprintf(V.msg, sizeof(V.msg), "\"%s\" %uL written", V.path, (uint32_t)V.nlines);
    } else {
        snprintf(V.msg, sizeof(V.msg), "Write error: %s (see serial)", V.path);
    }
    return r;
}

/* ── draw ─────────────────────────────────────────────────────────────────── */

static void draw(void)
{
    do_scroll();

    for (size_t sr = 0; sr < vim_rows; ++sr) {
        vga_goto(sr, 0);
        size_t fr = V.row_off + sr;
        if (fr < V.nlines) {
            const char *line = V.buf[fr];
            size_t len = strlen(line);
            for (size_t c = V.col_off; c < len && c < V.col_off + vim_cols; ++c)
                putchar(line[c]);
        } else {
            putchar('~');
        }
        vga_erase_eol();
    }

    /* status bar — row vim_rows, inverted colors */
    uint8_t saved_color = vga_color();
    vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
    vga_goto(vim_rows, 0);
    const char *mstr = (V.mode == INSERT) ? " -- INSERT -- " : " -- NORMAL -- ";
    printf("%s %s%s", mstr, V.path[0] ? V.path : "[No Name]", V.dirty ? " [+]" : "");
    vga_erase_eol();
    char rc[16];
    snprintf(rc, sizeof(rc), " %u:%-3u", (uint32_t)(V.cy + 1), (uint32_t)(V.cx + 1));
    size_t rlen = strlen(rc);
    vga_goto(vim_rows, rlen < vim_cols ? vim_cols - rlen : 0);
    printf("%s", rc);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* command / message line — row vim_rows + 1 */
    vga_goto(vim_rows + 1, 0);
    if (V.mode == COMMAND) {
        putchar(':');
        printf("%.*s", (int)V.cmdlen, V.cmd);
    } else if (V.mode == VSEARCH) {
        putchar('/');
        printf("%.*s", (int)V.search_len, V.search);
    } else if (V.pending) {
        putchar((char)V.pending);
    } else {
        printf("%s", V.msg);
    }
    vga_erase_eol();

    /* place hardware cursor */
    if (V.mode == COMMAND)       vga_set_cursor(vim_rows + 1, V.cmdlen + 1);
    else if (V.mode == VSEARCH)  vga_set_cursor(vim_rows + 1, V.search_len + 1);
    else {
        size_t crow = V.cy - V.row_off;
        size_t ccol = V.cx - V.col_off;
        if (crow < vim_rows) vga_set_cursor(crow, ccol);
    }
    (void)saved_color;
}

/* ── edit primitives ──────────────────────────────────────────────────────── */

static void insert_char(char c)
{
    char *line = V.buf[V.cy];
    size_t len = strlen(line);
    if (len + 1 >= VIM_MAX_COL) return;
    memmove(line + V.cx + 1, line + V.cx, len - V.cx + 1);
    line[V.cx++] = c;
    V.dirty = true;
}

static void insert_newline(void)
{
    if (V.nlines >= VIM_MAX_LINES) return;
    char *cur  = V.buf[V.cy];
    size_t len = strlen(cur);
    char *tail = lalloc(cur + V.cx, len - V.cx);
    if (!tail) return;
    cur[V.cx] = '\0';
    memmove(&V.buf[V.cy + 2], &V.buf[V.cy + 1],
            (V.nlines - V.cy - 1) * sizeof(char *));
    V.buf[V.cy + 1] = tail;
    V.nlines++;
    V.cy++;
    V.cx = 0;
    V.dirty = true;
}

static void backspace_char(void)
{
    if (V.cx > 0) {
        char *line = V.buf[V.cy];
        size_t len = strlen(line);
        memmove(line + V.cx - 1, line + V.cx, len - V.cx + 1);
        V.cx--;
    } else if (V.cy > 0) {
        size_t plen = strlen(V.buf[V.cy - 1]);
        size_t clen = strlen(V.buf[V.cy]);
        if (plen + clen < VIM_MAX_COL) {
            strcat(V.buf[V.cy - 1], V.buf[V.cy]);
            kfree(V.buf[V.cy]);
            memmove(&V.buf[V.cy], &V.buf[V.cy + 1],
                    (V.nlines - V.cy - 1) * sizeof(char *));
            V.nlines--;
            V.cy--;
            V.cx = plen;
        }
    }
    V.dirty = true;
}

static void delete_fwd(void)
{
    char *line = V.buf[V.cy];
    size_t len = strlen(line);
    if (V.cx < len) {
        memmove(line + V.cx, line + V.cx + 1, len - V.cx);
        V.dirty = true;
    } else if (V.cy + 1 < V.nlines) {
        size_t nlen = strlen(V.buf[V.cy + 1]);
        if (len + nlen < VIM_MAX_COL) {
            strcat(line, V.buf[V.cy + 1]);
            kfree(V.buf[V.cy + 1]);
            memmove(&V.buf[V.cy + 1], &V.buf[V.cy + 2],
                    (V.nlines - V.cy - 2) * sizeof(char *));
            V.nlines--;
            V.dirty = true;
        }
    }
}

static void delete_line(size_t r)
{
    if (V.nlines == 1) { V.buf[0][0] = '\0'; V.cx = 0; V.dirty = true; return; }
    kfree(V.buf[r]);
    memmove(&V.buf[r], &V.buf[r + 1], (V.nlines - r - 1) * sizeof(char *));
    V.nlines--;
    if (V.cy >= V.nlines) V.cy = V.nlines - 1;
    V.cx = 0;
    V.dirty = true;
}

static void delete_to_eol(void)
{
    char *line = V.buf[V.cy];
    size_t len = strlen(line);
    if (V.cx >= len) return;
    line[V.cx] = '\0';
    if (V.cx > 0) V.cx--;
    V.dirty = true;
}

static void yank_line(size_t r)
{
    strncpy(V.yank, V.buf[r], VIM_MAX_COL - 1);
    V.yank[VIM_MAX_COL - 1] = '\0';
    V.has_yank = true;
}

static void paste_below(void)
{
    if (!V.has_yank || V.nlines >= VIM_MAX_LINES) return;
    char *nl = lalloc(V.yank, strlen(V.yank));
    if (!nl) return;
    memmove(&V.buf[V.cy + 2], &V.buf[V.cy + 1],
            (V.nlines - V.cy - 1) * sizeof(char *));
    V.buf[V.cy + 1] = nl;
    V.nlines++;
    V.cy++;
    V.cx = 0;
    V.dirty = true;
}

static void paste_above(void)
{
    if (!V.has_yank || V.nlines >= VIM_MAX_LINES) return;
    char *nl = lalloc(V.yank, strlen(V.yank));
    if (!nl) return;
    memmove(&V.buf[V.cy + 1], &V.buf[V.cy],
            (V.nlines - V.cy) * sizeof(char *));
    V.buf[V.cy] = nl;
    V.nlines++;
    V.cx = 0;
    V.dirty = true;
}

/* ── word movement ────────────────────────────────────────────────────────── */

static bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void move_word_fwd(void)
{
    char *line = V.buf[V.cy];
    size_t len  = strlen(line);
    if (V.cx < len) {
        bool iw = is_word_char(line[V.cx]);
        while (V.cx < len && is_word_char(line[V.cx]) == iw) V.cx++;
        while (V.cx < len && line[V.cx] == ' ') V.cx++;
    } else if (V.cy + 1 < V.nlines) {
        V.cy++; V.cx = 0;
    }
}

static void move_word_back(void)
{
    if (V.cx > 0) {
        char *line = V.buf[V.cy];
        V.cx--;
        while (V.cx > 0 && line[V.cx] == ' ') V.cx--;
        bool iw = is_word_char(line[V.cx]);
        while (V.cx > 0 && is_word_char(line[V.cx - 1]) == iw) V.cx--;
    } else if (V.cy > 0) {
        V.cy--;
        size_t len = strlen(V.buf[V.cy]);
        V.cx = len > 0 ? len - 1 : 0;
    }
}

/* ── search ───────────────────────────────────────────────────────────────── */

static void search_next(void)
{
    if (!V.search_len) { set_msg("No previous search pattern"); return; }
    for (size_t i = 0; i <= V.nlines; ++i) {
        size_t ly   = (V.cy + i) % V.nlines;
        size_t from = (i == 0) ? (V.cx + 1) : 0;
        size_t llen = strlen(V.buf[ly]);
        if (from > llen) continue;
        char *p = strstr(V.buf[ly] + from, V.search);
        if (p) {
            V.cy = ly;
            V.cx = (size_t)(p - V.buf[ly]);
            set_msg("");
            return;
        }
    }
    set_msg("Pattern not found");
}

static void search_first(void)
{
    if (!V.search_len) return;
    for (size_t i = 0; i < V.nlines; ++i) {
        size_t ly = (V.cy + i) % V.nlines;
        char *p = strstr(V.buf[ly], V.search);
        if (p) { V.cy = ly; V.cx = (size_t)(p - V.buf[ly]); set_msg(""); return; }
    }
    set_msg("Pattern not found");
}

/* ── command execution ────────────────────────────────────────────────────── */

static void exec_cmd(void)
{
    V.cmd[V.cmdlen] = '\0';
    const char *c = V.cmd;
    if (strcmp(c, "w") == 0 || strcmp(c, "write") == 0) {
        save_file();
    } else if (strcmp(c, "q") == 0 || strcmp(c, "quit") == 0) {
        if (V.dirty) set_msg("No write since last change (use :q! to override)");
        else V.running = false;
    } else if (strcmp(c, "q!") == 0) {
        V.running = false;
    } else if (strcmp(c, "wq") == 0 || strcmp(c, "x") == 0 || strcmp(c, "wq!") == 0) {
        if (save_file() == 0) V.running = false;
    } else {
        size_t n = 0; bool is_num = (V.cmdlen > 0);
        for (size_t i = 0; i < V.cmdlen; ++i) {
            if (c[i] >= '0' && c[i] <= '9') n = n * 10 + (size_t)(c[i] - '0');
            else { is_num = false; break; }
        }
        if (is_num && n >= 1 && n <= V.nlines) { V.cy = n - 1; V.cx = 0; }
        else snprintf(V.msg, sizeof(V.msg), "Not an editor command: %s", c);
    }
    V.mode = NORMAL;
    V.cmdlen = 0;
}

/* ── mode handlers ────────────────────────────────────────────────────────── */

static void handle_normal(int key)
{
    V.msg[0] = '\0';
    char  *line = V.buf[V.cy];
    size_t len  = strlen(line);

    if (V.pending) {
        int p = V.pending; V.pending = 0;
        if (p == 'd') {
            if (key == 'd') { save_undo(); yank_line(V.cy); delete_line(V.cy); }
            else if (key == 'w') {
                save_undo();
                size_t start = V.cx, end = V.cx;
                if (start < len) {
                    bool iw = is_word_char(line[start]);
                    while (end < len && is_word_char(line[end]) == iw) end++;
                    while (end < len && line[end] == ' ') end++;
                    memmove(line + start, line + end, len - end + 1);
                    V.cx = start; V.dirty = true;
                }
            }
            else if (key == '$') { save_undo(); delete_to_eol(); }
            else if (key == '0') {
                save_undo(); line[0] = '\0'; V.cx = 0; V.dirty = true;
            }
        } else if (p == 'g') {
            if (key == 'g') { V.cy = 0; V.cx = 0; }
        } else if (p == 'y') {
            if (key == 'y') { yank_line(V.cy); set_msg("1 line yanked"); }
        } else if (p == 'r') {
            if (key >= 32 && key < 127 && V.cx < len) {
                save_undo(); line[V.cx] = (char)key; V.dirty = true;
            }
        } else if (p == 'c') {
            if (key == 'c') {
                save_undo(); yank_line(V.cy);
                line[0] = '\0'; V.cx = 0; V.dirty = true;
                V.mode = INSERT;
            } else if (key == 'w') {
                save_undo();
                size_t start = V.cx, end = V.cx;
                if (start < len) {
                    bool iw = is_word_char(line[start]);
                    while (end < len && is_word_char(line[end]) == iw) end++;
                    memmove(line + start, line + end, len - end + 1);
                    V.cx = start; V.dirty = true;
                }
                V.mode = INSERT;
            }
        }
        return;
    }

    switch (key) {
    /* ── movement ────────────────────────────── */
    case 'h': case KEY_LEFT:
        if (V.cx > 0) V.cx--;
        break;
    case 'l': case KEY_RIGHT:
        if (V.cx + 1 < len) V.cx++;
        break;
    case 'j': case KEY_DOWN:
        if (V.cy + 1 < V.nlines) { V.cy++; clamp_cx(); } break;
    case 'k': case KEY_UP:
        if (V.cy > 0) { V.cy--; clamp_cx(); } break;
    case '0': case KEY_HOME:
        V.cx = 0; break;
    case '$': case KEY_END:
        V.cx = len > 0 ? len - 1 : 0; break;
    case '^':
        V.cx = 0;
        while (V.cx < len && line[V.cx] == ' ') V.cx++;
        break;
    case 'w':
        move_word_fwd(); clamp_cx(); break;
    case 'b':
        move_word_back(); break;
    case 'G':
        V.cy = V.nlines - 1; V.cx = 0; break;
    case 'g':
        V.pending = 'g'; break;
    case KEY_PAGE_UP:
        V.cy = (V.cy >= vim_rows) ? V.cy - vim_rows : 0; clamp_cx(); break;
    case KEY_PAGE_DOWN:
        V.cy = (V.cy + vim_rows < V.nlines) ? V.cy + vim_rows : V.nlines - 1;
        clamp_cx(); break;
    /* ── enter insert mode ───────────────────── */
    case 'i':
        save_undo(); V.mode = INSERT; break;
    case 'a':
        save_undo(); if (V.cx < len) V.cx++; V.mode = INSERT; break;
    case 'A':
        save_undo(); V.cx = len; V.mode = INSERT; break;
    case 'I':
        save_undo();
        V.cx = 0;
        while (V.cx < len && line[V.cx] == ' ') V.cx++;
        V.mode = INSERT; break;
    case 'o':
        save_undo();
        if (V.nlines < VIM_MAX_LINES) {
            char *nl = lalloc("", 0);
            if (nl) {
                memmove(&V.buf[V.cy + 2], &V.buf[V.cy + 1],
                        (V.nlines - V.cy - 1) * sizeof(char *));
                V.buf[V.cy + 1] = nl; V.nlines++;
                V.cy++; V.cx = 0; V.dirty = true; V.mode = INSERT;
            }
        }
        break;
    case 'O':
        save_undo();
        if (V.nlines < VIM_MAX_LINES) {
            char *nl = lalloc("", 0);
            if (nl) {
                memmove(&V.buf[V.cy + 1], &V.buf[V.cy],
                        (V.nlines - V.cy) * sizeof(char *));
                V.buf[V.cy] = nl; V.nlines++;
                V.cx = 0; V.dirty = true; V.mode = INSERT;
            }
        }
        break;
    case 's':   /* substitute: delete char + insert */
        save_undo();
        if (len > 0 && V.cx < len) {
            memmove(line + V.cx, line + V.cx + 1, len - V.cx);
            V.dirty = true;
        }
        V.mode = INSERT; break;
    case 'S':   /* substitute whole line */
        save_undo(); yank_line(V.cy);
        line[0] = '\0'; V.cx = 0; V.dirty = true; V.mode = INSERT; break;
    /* ── editing ─────────────────────────────── */
    case 'x':
        if (len > 0 && V.cx < len) {
            save_undo();
            memmove(line + V.cx, line + V.cx + 1, len - V.cx);
            clamp_cx(); V.dirty = true;
        }
        break;
    case 'X':
        if (V.cx > 0) {
            save_undo();
            memmove(line + V.cx - 1, line + V.cx, len - V.cx + 1);
            V.cx--; V.dirty = true;
        }
        break;
    case 'd':
        V.pending = 'd'; break;
    case 'D':
        save_undo(); delete_to_eol(); break;
    case 'C':
        save_undo();
        line[V.cx] = '\0'; V.dirty = true;
        V.mode = INSERT; break;
    case 'c':
        V.pending = 'c'; break;
    case 'y':
        V.pending = 'y'; break;
    case 'p':
        save_undo(); paste_below(); break;
    case 'P':
        save_undo(); paste_above(); break;
    case 'r':
        V.pending = 'r'; break;
    case 'J':
        if (V.cy + 1 < V.nlines) {
            save_undo();
            size_t nlen = strlen(V.buf[V.cy + 1]);
            if (len + nlen + 1 < VIM_MAX_COL) {
                line[len] = ' ';
                memcpy(line + len + 1, V.buf[V.cy + 1], nlen + 1);
                kfree(V.buf[V.cy + 1]);
                memmove(&V.buf[V.cy + 1], &V.buf[V.cy + 2],
                        (V.nlines - V.cy - 2) * sizeof(char *));
                V.nlines--; V.cx = len; V.dirty = true;
            }
        }
        break;
    case 'u':
        do_undo(); break;
    /* ── search ──────────────────────────────── */
    case '/':
        V.mode = VSEARCH; V.search_len = 0; V.search[0] = '\0'; break;
    case 'n':
        search_next(); break;
    case 'N': {
        /* reverse search: scan backwards (simple: jump to prev occurrence) */
        if (!V.search_len) { set_msg("No previous search pattern"); break; }
        for (size_t i = 1; i <= V.nlines; ++i) {
            size_t ly = (V.cy + V.nlines - i) % V.nlines;
            size_t llen = strlen(V.buf[ly]);
            /* scan backwards through the line */
            if (llen == 0) continue;
            for (int col = (int)(ly == V.cy ? (int)V.cx - 1 : (int)llen - 1); col >= 0; col--) {
                char *p = strstr(V.buf[ly] + col, V.search);
                if (p && (size_t)(p - V.buf[ly]) == (size_t)col) {
                    V.cy = ly; V.cx = (size_t)col; set_msg(""); goto done_N;
                }
            }
        }
        set_msg("Pattern not found");
        done_N:; break;
    }
    /* ── command / save / quit ───────────────── */
    case ':':
        V.mode = COMMAND; V.cmdlen = 0; V.cmd[0] = '\0'; break;
    case KEY_CTRL_S:
        save_file(); break;
    case KEY_CTRL_Q:
        if (!V.dirty) V.running = false;
        else set_msg("Unsaved changes! Use :q! to force quit.");
        break;
    default:
        break;
    }
}

static void handle_insert(int key)
{
    switch (key) {
    case KEY_ESCAPE:
        V.mode = NORMAL;
        clamp_cx();
        break;
    case KEY_ENTER:
        insert_newline(); break;
    case KEY_BACKSPACE:
        backspace_char(); break;
    case KEY_DELETE:
        delete_fwd(); break;
    case KEY_UP:
        if (V.cy > 0) { V.cy--; clamp_cx(); } break;
    case KEY_DOWN:
        if (V.cy + 1 < V.nlines) { V.cy++; clamp_cx(); } break;
    case KEY_LEFT:
        if (V.cx > 0) V.cx--;
        break;
    case KEY_RIGHT: {
        size_t len = strlen(V.buf[V.cy]);
        if (V.cx < len) V.cx++;
        break;
    }
    case KEY_HOME:
        V.cx = 0; break;
    case KEY_END:
        V.cx = strlen(V.buf[V.cy]); break;
    case KEY_TAB:
        for (int i = 0; i < 4; ++i) insert_char(' ');
        break;
    default:
        if (key >= 32 && key < 127) insert_char((char)key);
        break;
    }
}

static void handle_command(int key)
{
    switch (key) {
    case KEY_ESCAPE:
        V.mode = NORMAL; V.cmdlen = 0; break;
    case KEY_ENTER:
        exec_cmd(); break;
    case KEY_BACKSPACE:
        if (V.cmdlen > 0) V.cmdlen--;
        else V.mode = NORMAL;
        break;
    default:
        if (key >= 32 && key < 127 && V.cmdlen + 1 < sizeof(V.cmd))
            V.cmd[V.cmdlen++] = (char)key;
        break;
    }
}

static void handle_search(int key)
{
    switch (key) {
    case KEY_ESCAPE:
        V.mode = NORMAL; V.search_len = 0; break;
    case KEY_ENTER:
        V.search[V.search_len] = '\0';
        V.mode = NORMAL;
        if (V.search_len > 0) search_first();
        break;
    case KEY_BACKSPACE:
        if (V.search_len > 0) V.search[--V.search_len] = '\0';
        else V.mode = NORMAL;
        break;
    default:
        if (key >= 32 && key < 127 && V.search_len + 1 < sizeof(V.search)) {
            V.search[V.search_len++] = (char)key;
            V.search[V.search_len]   = '\0';
        }
        break;
    }
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int editor_open(const char *path)
{
    size_t scr_rows, scr_cols;
    vga_get_size(&scr_rows, &scr_cols);
    vim_rows = (scr_rows > 2) ? scr_rows - 2 : 1;
    vim_cols = scr_cols;

    memset(&V, 0, sizeof(V));
    V.running = true;
    V.mode    = NORMAL;
    load_file(path);
    vga_clear();

    while (V.running) {
        draw();
        int key = keyboard_getkey();
        switch (V.mode) {
        case NORMAL:  handle_normal(key);  break;
        case INSERT:  handle_insert(key);  break;
        case COMMAND: handle_command(key); break;
        case VSEARCH: handle_search(key);  break;
        }
    }

    buf_free();
    if (V.has_undo)
        for (size_t i = 0; i < V.unlines; ++i) kfree(V.ubuf[i]);
    vga_clear();
    return 0;
}
