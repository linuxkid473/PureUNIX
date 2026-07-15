/* user/pude_text.c -- PUText, a real ring-3 graphical text editor for
 * `pude` (see user/pude_text.h and docs/pude.md's "PUText" section for the
 * full design writeup). Plugs into the WM through the same app_class_t
 * (user/pude_app.h) PUTerm/Calculator/PUFiles use.
 *
 * Document buffer: a dynamic array of dynamically-grown lines (pt_doc_t /
 * pt_line_t below) -- not a fixed-size buffer, not neatvi's lbuf.c (that's
 * built around ex/regex/undo machinery this editor doesn't need; inspected
 * for ideas, not embedded). Every line grows via realloc doubling, and the
 * line array itself grows the same way, so there is no built-in ceiling on
 * line length or document size beyond real available heap (the ring-3
 * sbrk() heap is a real incrementally-grown 32 MiB region, see
 * include/pureunix/vmm.h -- more than enough for any reasonable text file,
 * with nothing PUText-specific needed to get there).
 *
 * Cursor/selection: (row,col) byte-offset pairs; a selection is just an
 * anchor (row,col) plus the live cursor, normalized on demand -- no
 * separate "selection object" to keep in sync.
 *
 * Clipboard: pude_clipboard.h's real cross-app clipboard, not private
 * per-instance state -- Ctrl+C in one PUText window and Ctrl+V in another
 * (or a future app) round-trips correctly.
 *
 * File picker: the embeddable pude_filepicker.h widget, drawn as a modal
 * over PUText's own client area exactly like PUFiles' New Folder/Rename/
 * Delete dialogs are drawn over PUFiles' own.
 *
 * Caret: solid, not blinking. There is no damage-rect compositor in this
 * tree (user/pude.c redraws the whole desktop surface whenever anything
 * changes) -- a periodic blink would mean PUText forcing a full-screen
 * repaint every ~500ms purely to toggle a caret, for as long as the window
 * is open, which is exactly the kind of needless redraw traffic this
 * project has fought before (see the project's own cursor-lag history). A
 * solid caret sidesteps the problem by construction: PUText only ever
 * causes a redraw in response to real input (typing, cursor movement,
 * mouse activity), exactly like every other app in this desktop already
 * behaves -- zero *additional* redraw traffic from having a caret at all.
 */
#include "pude_text.h"
#include "pude_clipboard.h"
#include "pude_filepicker.h"
#include "pude_gfx.h"
#include "pude_widgets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_PATH_MAX 256

#define PT_TOOLBAR_H 30
#define PT_STATUS_H  20
#define PT_LEFT_MARGIN 4

/* ============================================================================
 * Document buffer: dynamic array of dynamically-grown lines.
 * ========================================================================= */

typedef struct {
    char *buf;
    int len;
    int cap;
} pt_line_t;

typedef struct {
    pt_line_t *lines;
    int line_count;
    int line_cap;
    bool trailing_newline; /* whether the on-disk file should end with \n */
} pt_doc_t;

static void pt_line_init(pt_line_t *ln)
{
    ln->buf = NULL;
    ln->len = 0;
    ln->cap = 0;
}

static void pt_line_free(pt_line_t *ln)
{
    free(ln->buf);
    ln->buf = NULL;
    ln->len = 0;
    ln->cap = 0;
}

static void pt_line_ensure_cap(pt_line_t *ln, int need)
{
    if (need <= ln->cap) {
        return;
    }
    int newcap = ln->cap ? ln->cap * 2 : 32;
    while (newcap < need) {
        newcap *= 2;
    }
    ln->buf = realloc(ln->buf, (size_t)newcap);
    ln->cap = newcap;
}

static void pt_line_set(pt_line_t *ln, const char *s, int len)
{
    pt_line_ensure_cap(ln, len);
    if (len > 0) {
        memcpy(ln->buf, s, (size_t)len);
    }
    ln->len = len;
}

/* Inserts `slen` bytes at byte offset `col` (0 <= col <= ln->len). */
static void pt_line_insert(pt_line_t *ln, int col, const char *s, int slen)
{
    if (slen <= 0) {
        return;
    }
    pt_line_ensure_cap(ln, ln->len + slen);
    memmove(ln->buf + col + slen, ln->buf + col, (size_t)(ln->len - col));
    memcpy(ln->buf + col, s, (size_t)slen);
    ln->len += slen;
}

/* Deletes the half-open byte range [col0, col1). */
static void pt_line_delete(pt_line_t *ln, int col0, int col1)
{
    int n = col1 - col0;
    if (n <= 0) {
        return;
    }
    memmove(ln->buf + col0, ln->buf + col1, (size_t)(ln->len - col1));
    ln->len -= n;
}

static void pt_doc_ensure_line_cap(pt_doc_t *doc, int need)
{
    if (need <= doc->line_cap) {
        return;
    }
    int newcap = doc->line_cap ? doc->line_cap * 2 : 64;
    while (newcap < need) {
        newcap *= 2;
    }
    doc->lines = realloc(doc->lines, (size_t)newcap * sizeof(pt_line_t));
    doc->line_cap = newcap;
}

/* Inserts one fresh empty line at index `idx`, shifting later lines right. */
static void pt_doc_insert_line_at(pt_doc_t *doc, int idx)
{
    pt_doc_ensure_line_cap(doc, doc->line_count + 1);
    memmove(&doc->lines[idx + 1], &doc->lines[idx],
            (size_t)(doc->line_count - idx) * sizeof(pt_line_t));
    pt_line_init(&doc->lines[idx]);
    doc->line_count++;
}

static void pt_doc_remove_line_at(pt_doc_t *doc, int idx)
{
    pt_line_free(&doc->lines[idx]);
    memmove(&doc->lines[idx], &doc->lines[idx + 1],
            (size_t)(doc->line_count - idx - 1) * sizeof(pt_line_t));
    doc->line_count--;
}

static void pt_doc_init_empty(pt_doc_t *doc)
{
    memset(doc, 0, sizeof(*doc));
    pt_doc_ensure_line_cap(doc, 1);
    pt_line_init(&doc->lines[0]);
    doc->line_count = 1;
    doc->trailing_newline = false;
}

static void pt_doc_free(pt_doc_t *doc)
{
    for (int i = 0; i < doc->line_count; i++) {
        pt_line_free(&doc->lines[i]);
    }
    free(doc->lines);
    memset(doc, 0, sizeof(*doc));
}

/* Splits line `row` at column `col`: the line keeps [0,col), a brand new
 * line row+1 gets [col,end). Used for Enter and for pasting text that
 * contains embedded newlines. */
static void pt_doc_split_line(pt_doc_t *doc, int row, int col)
{
    pt_line_t *ln = &doc->lines[row];
    int taillen = ln->len - col;
    char *tail = NULL;
    if (taillen > 0) {
        tail = malloc((size_t)taillen);
        memcpy(tail, ln->buf + col, (size_t)taillen);
    }
    pt_doc_insert_line_at(doc, row + 1); /* may realloc doc->lines -- re-fetch below */
    ln = &doc->lines[row];
    ln->len = col;
    if (taillen > 0) {
        pt_line_insert(&doc->lines[row + 1], 0, tail, taillen);
        free(tail);
    }
}

/* Joins line row+1 onto the end of line row, then removes row+1 -- the
 * general merge Backspace-at-column-0 and Delete-at-end-of-line both need. */
static void pt_doc_join_line(pt_doc_t *doc, int row)
{
    pt_line_t *a = &doc->lines[row];
    pt_line_t *b = &doc->lines[row + 1];
    if (b->len > 0) {
        pt_line_insert(a, a->len, b->buf, b->len);
    }
    pt_doc_remove_line_at(doc, row + 1);
}

/* Deletes the (already row/col-normalized: (r0,c0) <= (r1,c1)) range
 * [r0,c0 .. r1,c1). Handles both a same-line and a multi-line range. */
static void pt_doc_delete_range(pt_doc_t *doc, int r0, int c0, int r1, int c1)
{
    if (r0 == r1) {
        pt_line_delete(&doc->lines[r0], c0, c1);
        return;
    }
    pt_line_t *first = &doc->lines[r0];
    pt_line_t *last = &doc->lines[r1];
    int taillen = last->len - c1;
    first->len = c0;
    if (taillen > 0) {
        pt_line_insert(first, first->len, last->buf + c1, taillen);
    }
    for (int i = r1; i > r0; i--) {
        pt_doc_remove_line_at(doc, i);
    }
}

/* Inserts arbitrary text (possibly containing '\n') at (row,col), splitting
 * lines as needed -- the one general primitive both typed-Enter and a
 * multi-line paste use. Writes the cursor position immediately after the
 * inserted text to out_row/out_col. */
static void pt_doc_insert_text(pt_doc_t *doc, int row, int col, const char *text, int textlen,
                                int *out_row, int *out_col)
{
    int i = 0;
    int cur_row = row, cur_col = col;
    while (i < textlen) {
        int start = i;
        while (i < textlen && text[i] != '\n') {
            i++;
        }
        int seglen = i - start;
        pt_line_insert(&doc->lines[cur_row], cur_col, text + start, seglen);
        cur_col += seglen;
        if (i < textlen && text[i] == '\n') {
            pt_doc_split_line(doc, cur_row, cur_col);
            cur_row++;
            cur_col = 0;
            i++;
        }
    }
    *out_row = cur_row;
    *out_col = cur_col;
}

/* Extracts [r0,c0 .. r1,c1) as a single malloc'd buffer (embedded lines
 * joined with '\n'), for Copy/Cut. Caller frees. */
static char *pt_doc_get_range(const pt_doc_t *doc, int r0, int c0, int r1, int c1, int *out_len)
{
    if (r0 == r1) {
        int n = c1 - c0;
        char *out = malloc((size_t)(n > 0 ? n : 1));
        if (n > 0) {
            memcpy(out, doc->lines[r0].buf + c0, (size_t)n);
        }
        *out_len = n;
        return out;
    }
    size_t total = (size_t)(doc->lines[r0].len - c0) + 1;
    for (int r = r0 + 1; r < r1; r++) {
        total += (size_t)doc->lines[r].len + 1;
    }
    total += (size_t)c1;
    char *out = malloc(total > 0 ? total : 1);
    size_t o = 0;
    memcpy(out + o, doc->lines[r0].buf + c0, (size_t)(doc->lines[r0].len - c0));
    o += (size_t)(doc->lines[r0].len - c0);
    out[o++] = '\n';
    for (int r = r0 + 1; r < r1; r++) {
        memcpy(out + o, doc->lines[r].buf, (size_t)doc->lines[r].len);
        o += (size_t)doc->lines[r].len;
        out[o++] = '\n';
    }
    memcpy(out + o, doc->lines[r1].buf, (size_t)c1);
    o += (size_t)c1;
    *out_len = (int)o;
    return out;
}

/* Loads `path` into a brand-new pt_doc_t (the caller's existing doc, if
 * any, is untouched until this succeeds -- a failed Open must never lose
 * the document currently being edited). Handles an empty file (one empty
 * line, no trailing newline), a file with no trailing newline, and any
 * number of embedded/consecutive newlines uniformly by splitting on '\n'. */
static bool pt_doc_load(pt_doc_t *doc, const char *path, char *errbuf, size_t errcap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(errbuf, errcap, "%s", strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(errbuf, errcap, "%s", strerror(errno));
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(errbuf, errcap, "could not determine file size");
        return false;
    }

    char *data = NULL;
    if (sz > 0) {
        data = malloc((size_t)sz);
        if (!data) {
            fclose(f);
            snprintf(errbuf, errcap, "out of memory");
            return false;
        }
        size_t rd = fread(data, 1, (size_t)sz, f);
        if ((long)rd != sz) {
            free(data);
            fclose(f);
            snprintf(errbuf, errcap, "read error");
            return false;
        }
    }
    fclose(f);

    pt_doc_t newdoc;
    memset(&newdoc, 0, sizeof(newdoc));
    if (sz == 0) {
        pt_doc_ensure_line_cap(&newdoc, 1);
        pt_line_init(&newdoc.lines[0]);
        newdoc.line_count = 1;
        newdoc.trailing_newline = false;
    } else {
        bool ends_nl = data[sz - 1] == '\n';
        long content_len = ends_nl ? sz - 1 : sz;
        long line_start = 0;
        for (long i = 0; i <= content_len; i++) {
            if (i == content_len || data[i] == '\n') {
                pt_doc_ensure_line_cap(&newdoc, newdoc.line_count + 1);
                pt_line_t *ln = &newdoc.lines[newdoc.line_count++];
                pt_line_init(ln);
                int llen = (int)(i - line_start);
                if (llen > 0) {
                    pt_line_set(ln, data + line_start, llen);
                }
                line_start = i + 1;
            }
        }
        newdoc.trailing_newline = ends_nl;
    }
    free(data);
    *doc = newdoc;
    return true;
}

/* Writes `doc` to `path`, reproducing exactly the trailing-newline
 * convention it was loaded with (or false for a brand new document, so a
 * fresh empty document saves as a real 0-byte file). */
static bool pt_doc_save(const pt_doc_t *doc, const char *path, char *errbuf, size_t errcap)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(errbuf, errcap, "%s", strerror(errno));
        return false;
    }
    for (int i = 0; i < doc->line_count; i++) {
        const pt_line_t *ln = &doc->lines[i];
        if (ln->len > 0 && fwrite(ln->buf, 1, (size_t)ln->len, f) != (size_t)ln->len) {
            fclose(f);
            snprintf(errbuf, errcap, "write error");
            return false;
        }
        bool want_nl = (i < doc->line_count - 1) || doc->trailing_newline;
        if (want_nl && fputc('\n', f) == EOF) {
            fclose(f);
            snprintf(errbuf, errcap, "write error");
            return false;
        }
    }
    if (fclose(f) != 0) {
        snprintf(errbuf, errcap, "%s", strerror(errno));
        return false;
    }
    return true;
}

/* ============================================================================
 * App state.
 * ========================================================================= */

typedef enum { PT_MODE_EDIT, PT_MODE_CONFIRM_DISCARD, PT_MODE_CONFIRM_CLOSE, PT_MODE_FILEPICKER } pt_mode_t;
typedef enum { PT_PENDING_NONE, PT_PENDING_NEW, PT_PENDING_OPEN } pt_pending_t;

typedef struct {
    pt_doc_t doc;

    int cursor_row, cursor_col;
    int desired_col; /* goal column for Up/Down across shorter lines */

    bool has_selection;
    int sel_anchor_row, sel_anchor_col;

    int scroll_top; /* first visible line index */
    int scroll_col; /* first visible column (chars) */

    int cw, ch; /* current client size */

    char filepath[PT_PATH_MAX];
    bool has_filepath;
    bool modified;

    char status_msg[192];
    bool status_is_error;

    pt_mode_t mode;
    pt_pending_t pending_action;
    pu_filepicker_t picker;

    bool mouse_selecting;
} putext_state_t;

static char g_startup_path[PT_PATH_MAX];

void putext_set_startup_path(const char *path)
{
    if (path && path[0]) {
        strncpy(g_startup_path, path, sizeof(g_startup_path) - 1);
        g_startup_path[sizeof(g_startup_path) - 1] = '\0';
    } else {
        g_startup_path[0] = '\0';
    }
}

/* ---- small helpers ---------------------------------------------------- */

static void pt_status_info(putext_state_t *st, const char *msg)
{
    strncpy(st->status_msg, msg, sizeof(st->status_msg) - 1);
    st->status_msg[sizeof(st->status_msg) - 1] = '\0';
    st->status_is_error = false;
}

static void pt_status_error(putext_state_t *st, const char *msg)
{
    strncpy(st->status_msg, msg, sizeof(st->status_msg) - 1);
    st->status_msg[sizeof(st->status_msg) - 1] = '\0';
    st->status_is_error = true;
}

static void pt_dirname(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        strncpy(out, "/", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static int pt_text_area_h(const putext_state_t *st)
{
    int h = st->ch - PT_TOOLBAR_H - PT_STATUS_H;
    return h > 0 ? h : 0;
}

static int pt_visible_rows(const putext_state_t *st)
{
    int r = pt_text_area_h(st) / FONT_CELL_H;
    return r > 0 ? r : 1;
}

static int pt_visible_cols(const putext_state_t *st)
{
    int w = st->cw - PT_LEFT_MARGIN;
    int c = w > 0 ? w / FONT_CELL_W : 0;
    return c > 0 ? c : 1;
}

static void pt_clamp_cursor(putext_state_t *st)
{
    if (st->cursor_row < 0) st->cursor_row = 0;
    if (st->cursor_row >= st->doc.line_count) st->cursor_row = st->doc.line_count - 1;
    int llen = st->doc.lines[st->cursor_row].len;
    if (st->cursor_col < 0) st->cursor_col = 0;
    if (st->cursor_col > llen) st->cursor_col = llen;
}

static void pt_ensure_visible(putext_state_t *st)
{
    int vrows = pt_visible_rows(st);
    int vcols = pt_visible_cols(st);
    if (st->cursor_row < st->scroll_top) {
        st->scroll_top = st->cursor_row;
    }
    if (st->cursor_row >= st->scroll_top + vrows) {
        st->scroll_top = st->cursor_row - vrows + 1;
    }
    int max_top = st->doc.line_count - vrows;
    if (max_top < 0) max_top = 0;
    if (st->scroll_top > max_top) st->scroll_top = max_top;
    if (st->scroll_top < 0) st->scroll_top = 0;

    if (st->cursor_col < st->scroll_col) {
        st->scroll_col = st->cursor_col;
    }
    if (st->cursor_col >= st->scroll_col + vcols) {
        st->scroll_col = st->cursor_col - vcols + 1;
    }
    if (st->scroll_col < 0) st->scroll_col = 0;
}

/* Normalizes anchor/cursor into an ordered (r0,c0) <= (r1,c1) range. */
static void pt_sel_range(const putext_state_t *st, int *r0, int *c0, int *r1, int *c1)
{
    int ar = st->sel_anchor_row, ac = st->sel_anchor_col;
    int cr = st->cursor_row, cc = st->cursor_col;
    if (ar < cr || (ar == cr && ac <= cc)) {
        *r0 = ar; *c0 = ac; *r1 = cr; *c1 = cc;
    } else {
        *r0 = cr; *c0 = cc; *r1 = ar; *c1 = ac;
    }
}

/* ---- editing operations ------------------------------------------------ */

static void pt_delete_selection(putext_state_t *st)
{
    if (!st->has_selection) {
        return;
    }
    int r0, c0, r1, c1;
    pt_sel_range(st, &r0, &c0, &r1, &c1);
    pt_doc_delete_range(&st->doc, r0, c0, r1, c1);
    st->cursor_row = r0;
    st->cursor_col = c0;
    st->has_selection = false;
    st->modified = true;
}

static void pt_insert_char(putext_state_t *st, char c)
{
    if (st->has_selection) {
        pt_delete_selection(st);
    }
    pt_line_insert(&st->doc.lines[st->cursor_row], st->cursor_col, &c, 1);
    st->cursor_col++;
    st->desired_col = st->cursor_col;
    st->modified = true;
}

static void pt_insert_newline(putext_state_t *st)
{
    if (st->has_selection) {
        pt_delete_selection(st);
    }
    pt_doc_split_line(&st->doc, st->cursor_row, st->cursor_col);
    st->cursor_row++;
    st->cursor_col = 0;
    st->desired_col = 0;
    st->modified = true;
}

static void pt_insert_text(putext_state_t *st, const char *text, int len)
{
    if (len <= 0) {
        return;
    }
    if (st->has_selection) {
        pt_delete_selection(st);
    }
    int nr, nc;
    pt_doc_insert_text(&st->doc, st->cursor_row, st->cursor_col, text, len, &nr, &nc);
    st->cursor_row = nr;
    st->cursor_col = nc;
    st->desired_col = nc;
    st->modified = true;
}

static void pt_backspace(putext_state_t *st)
{
    if (st->has_selection) {
        pt_delete_selection(st);
        return;
    }
    if (st->cursor_col > 0) {
        pt_line_delete(&st->doc.lines[st->cursor_row], st->cursor_col - 1, st->cursor_col);
        st->cursor_col--;
    } else if (st->cursor_row > 0) {
        int prevlen = st->doc.lines[st->cursor_row - 1].len;
        pt_doc_join_line(&st->doc, st->cursor_row - 1);
        st->cursor_row--;
        st->cursor_col = prevlen;
    } else {
        return;
    }
    st->desired_col = st->cursor_col;
    st->modified = true;
}

static void pt_delete_forward(putext_state_t *st)
{
    if (st->has_selection) {
        pt_delete_selection(st);
        return;
    }
    pt_line_t *ln = &st->doc.lines[st->cursor_row];
    if (st->cursor_col < ln->len) {
        pt_line_delete(ln, st->cursor_col, st->cursor_col + 1);
    } else if (st->cursor_row < st->doc.line_count - 1) {
        pt_doc_join_line(&st->doc, st->cursor_row);
    } else {
        return;
    }
    st->modified = true;
}

static void pt_select_all(putext_state_t *st)
{
    st->sel_anchor_row = 0;
    st->sel_anchor_col = 0;
    st->cursor_row = st->doc.line_count - 1;
    st->cursor_col = st->doc.lines[st->cursor_row].len;
    st->has_selection = true;
    pt_ensure_visible(st);
}

static void pt_copy(putext_state_t *st)
{
    if (!st->has_selection) {
        return;
    }
    int r0, c0, r1, c1;
    pt_sel_range(st, &r0, &c0, &r1, &c1);
    int len;
    char *text = pt_doc_get_range(&st->doc, r0, c0, r1, c1, &len);
    pude_clipboard_set(text, (size_t)len);
    free(text);
}

static void pt_cut(putext_state_t *st)
{
    if (!st->has_selection) {
        return;
    }
    pt_copy(st);
    pt_delete_selection(st);
}

static void pt_paste(putext_state_t *st)
{
    size_t len;
    const char *text = pude_clipboard_get(&len);
    if (!text || len == 0) {
        return;
    }
    pt_insert_text(st, text, (int)len);
    pt_ensure_visible(st);
}

/* ---- cursor movement (all shift-selection aware via a single pattern) -- */

static void pt_move_cursor_to(putext_state_t *st, int new_row, int new_col, bool extend, bool set_desired)
{
    if (extend) {
        if (!st->has_selection) {
            st->sel_anchor_row = st->cursor_row;
            st->sel_anchor_col = st->cursor_col;
            st->has_selection = true;
        }
    } else {
        st->has_selection = false;
    }
    st->cursor_row = new_row;
    st->cursor_col = new_col;
    pt_clamp_cursor(st);
    if (set_desired) {
        st->desired_col = st->cursor_col;
    }
    pt_ensure_visible(st);
}

static void pt_move_left(putext_state_t *st, bool extend)
{
    int row = st->cursor_row, col = st->cursor_col;
    if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = st->doc.lines[row].len;
    }
    pt_move_cursor_to(st, row, col, extend, true);
}

static void pt_move_right(putext_state_t *st, bool extend)
{
    int row = st->cursor_row, col = st->cursor_col;
    pt_line_t *ln = &st->doc.lines[row];
    if (col < ln->len) {
        col++;
    } else if (row < st->doc.line_count - 1) {
        row++;
        col = 0;
    }
    pt_move_cursor_to(st, row, col, extend, true);
}

static void pt_move_vert(putext_state_t *st, int delta, bool extend)
{
    int row = st->cursor_row + delta;
    if (row < 0) row = 0;
    if (row >= st->doc.line_count) row = st->doc.line_count - 1;
    int col = st->desired_col;
    int llen = st->doc.lines[row].len;
    if (col > llen) col = llen;
    pt_move_cursor_to(st, row, col, extend, false);
}

static void pt_move_home(putext_state_t *st, bool extend)
{
    pt_move_cursor_to(st, st->cursor_row, 0, extend, true);
}

static void pt_move_end(putext_state_t *st, bool extend)
{
    pt_move_cursor_to(st, st->cursor_row, st->doc.lines[st->cursor_row].len, extend, true);
}

static void pt_move_page(putext_state_t *st, int dir, bool extend)
{
    pt_move_vert(st, dir * pt_visible_rows(st), extend);
}

/* ---- mouse cursor placement --------------------------------------------- */

static void pt_row_col_at(const putext_state_t *st, int x, int y, int *out_row, int *out_col)
{
    int rel_y = y - PT_TOOLBAR_H;
    if (rel_y < 0) rel_y = 0;
    int row = st->scroll_top + rel_y / FONT_CELL_H;
    if (row < 0) row = 0;
    if (row >= st->doc.line_count) row = st->doc.line_count - 1;

    int rel_x = x - PT_LEFT_MARGIN;
    if (rel_x < 0) rel_x = 0;
    int col = st->scroll_col + rel_x / FONT_CELL_W;
    if (col < 0) col = 0;
    int llen = st->doc.lines[row].len;
    if (col > llen) col = llen;

    *out_row = row;
    *out_col = col;
}

/* ---- toolbar / modal layout ---------------------------------------------- */

static void pt_toolbar_buttons(pu_button_t out[4])
{
    static const char *labels[4] = { "New", "Open", "Save", "Save As" };
    int bw = 78, gap = 4;
    for (int i = 0; i < 4; i++) {
        out[i].x = 4 + i * (bw + gap);
        out[i].y = 3;
        out[i].w = bw;
        out[i].h = PT_TOOLBAR_H - 6;
        out[i].label = labels[i];
    }
}

static void pt_modal_rect(const putext_state_t *st, int *dx, int *dy, int *dw, int *dh)
{
    int w = 440;
    if (w > st->cw - 16) w = (st->cw - 16 > 60) ? st->cw - 16 : 60;
    int h = 340;
    if (h > st->ch - 16) h = (st->ch - 16 > 80) ? st->ch - 16 : 80;
    *dw = w; *dh = h;
    *dx = (st->cw - w) / 2;
    *dy = (st->ch - h) / 2;
}

static void pt_confirm_rect(const putext_state_t *st, int *dx, int *dy, int *dw, int *dh)
{
    int w = 360;
    if (w > st->cw - 8) w = (st->cw - 8 > 60) ? st->cw - 8 : 60;
    int h = 120;
    if (h > st->ch - 8) h = (st->ch - 8 > 60) ? st->ch - 8 : 60;
    *dw = w; *dh = h;
    *dx = (st->cw - w) / 2;
    *dy = (st->ch - h) / 2;
}

static void pt_confirm_buttons(const putext_state_t *st, const char *left_label,
                                const char *right_label, pu_button_t *left, pu_button_t *right)
{
    int dx, dy, dw, dh;
    pt_confirm_rect(st, &dx, &dy, &dw, &dh);
    int bw = 130, bh = 26, gap = 16;
    int total = bw * 2 + gap;
    int bx0 = dx + (dw - total) / 2;
    int by = dy + dh - bh - 12;
    left->x = bx0; left->y = by; left->w = bw; left->h = bh; left->label = left_label;
    right->x = bx0 + bw + gap; right->y = by; right->w = bw; right->h = bh; right->label = right_label;
}

/* ---- New / Open / Save / Save As actions --------------------------------- */

static void pt_do_new(putext_state_t *st)
{
    pt_doc_free(&st->doc);
    pt_doc_init_empty(&st->doc);
    st->cursor_row = 0; st->cursor_col = 0; st->desired_col = 0;
    st->has_selection = false;
    st->scroll_top = 0; st->scroll_col = 0;
    st->has_filepath = false;
    st->filepath[0] = '\0';
    st->modified = false;
    st->mode = PT_MODE_EDIT;
    pt_status_info(st, "New document");
}

static void pt_begin_open_picker(putext_state_t *st)
{
    char startdir[PT_PATH_MAX];
    if (st->has_filepath) {
        pt_dirname(st->filepath, startdir, sizeof(startdir));
    } else {
        strcpy(startdir, "/");
    }
    pu_filepicker_open_init(&st->picker, startdir);
    st->mode = PT_MODE_FILEPICKER;
}

static void pt_begin_save_picker(putext_state_t *st)
{
    char startdir[PT_PATH_MAX];
    char fname[PU_FP_NAME_MAX] = "";
    if (st->has_filepath) {
        pt_dirname(st->filepath, startdir, sizeof(startdir));
        const char *base = strrchr(st->filepath, '/');
        base = base ? base + 1 : st->filepath;
        strncpy(fname, base, sizeof(fname) - 1);
    } else {
        strcpy(startdir, "/");
    }
    pu_filepicker_save_init(&st->picker, startdir, fname);
    st->mode = PT_MODE_FILEPICKER;
}

static void pt_do_save_to(putext_state_t *st, const char *path)
{
    char err[128];
    if (!pt_doc_save(&st->doc, path, err, sizeof(err))) {
        char msg[192];
        snprintf(msg, sizeof(msg), "save failed: %s", err);
        pt_status_error(st, msg);
        return;
    }
    strncpy(st->filepath, path, sizeof(st->filepath) - 1);
    st->filepath[sizeof(st->filepath) - 1] = '\0';
    st->has_filepath = true;
    st->modified = false;
    char msg[192];
    snprintf(msg, sizeof(msg), "saved %s", path);
    pt_status_info(st, msg);
}

static void pt_action_new(putext_state_t *st)
{
    if (st->modified) {
        st->mode = PT_MODE_CONFIRM_DISCARD;
        st->pending_action = PT_PENDING_NEW;
        return;
    }
    pt_do_new(st);
}

static void pt_action_open(putext_state_t *st)
{
    if (st->modified) {
        st->mode = PT_MODE_CONFIRM_DISCARD;
        st->pending_action = PT_PENDING_OPEN;
        return;
    }
    pt_begin_open_picker(st);
}

static void pt_action_save(putext_state_t *st)
{
    if (st->has_filepath) {
        pt_do_save_to(st, st->filepath);
        return;
    }
    pt_begin_save_picker(st);
}

static void pt_action_save_as(putext_state_t *st)
{
    pt_begin_save_picker(st);
}

static void pt_toolbar_action(putext_state_t *st, int idx)
{
    switch (idx) {
    case 0: pt_action_new(st); break;
    case 1: pt_action_open(st); break;
    case 2: pt_action_save(st); break;
    case 3: pt_action_save_as(st); break;
    default: break;
    }
}

static void pt_discard_confirmed(putext_state_t *st)
{
    pt_pending_t action = st->pending_action;
    st->pending_action = PT_PENDING_NONE;
    if (action == PT_PENDING_NEW) {
        pt_do_new(st);
    } else if (action == PT_PENDING_OPEN) {
        pt_begin_open_picker(st);
    } else {
        st->mode = PT_MODE_EDIT;
    }
}

static void pt_filepicker_confirm(putext_state_t *st)
{
    char path[PT_PATH_MAX];
    if (!pu_filepicker_result_path(&st->picker, path, sizeof(path))) {
        pt_status_error(st, "path too long");
        st->mode = PT_MODE_EDIT;
        return;
    }
    if (st->picker.mode == PU_FP_MODE_OPEN) {
        char err[128];
        pt_doc_t newdoc;
        if (!pt_doc_load(&newdoc, path, err, sizeof(err))) {
            char msg[192];
            snprintf(msg, sizeof(msg), "open failed: %s", err);
            pt_status_error(st, msg);
            st->mode = PT_MODE_EDIT;
            return;
        }
        pt_doc_free(&st->doc);
        st->doc = newdoc;
        st->cursor_row = 0; st->cursor_col = 0; st->desired_col = 0;
        st->has_selection = false;
        st->scroll_top = 0; st->scroll_col = 0;
        strncpy(st->filepath, path, sizeof(st->filepath) - 1);
        st->filepath[sizeof(st->filepath) - 1] = '\0';
        st->has_filepath = true;
        st->modified = false;
        char msg[192];
        snprintf(msg, sizeof(msg), "opened %s", path);
        pt_status_info(st, msg);
        st->mode = PT_MODE_EDIT;
    } else {
        pt_do_save_to(st, path);
        st->mode = PT_MODE_EDIT;
    }
}

/* ============================================================================
 * Rendering.
 * ========================================================================= */

static void pt_draw_confirm(SDL_Surface *s, const putext_state_t *st, int cx, int cy,
                            const char *message, const char *right_label)
{
    int dx, dy, dw, dh;
    pt_confirm_rect(st, &dx, &dy, &dw, &dh);
    dx += cx; dy += cy;
    Uint32 box_bg = SDL_MapRGB(s->format, 45, 48, 58);
    Uint32 border = SDL_MapRGB(s->format, 170, 175, 185);
    pu_fill_rect(s, dx, dy, dw, dh, box_bg);
    pu_draw_rect_outline(s, dx, dy, dw, dh, 2, border);
    pu_draw_string_clipped(s, dx + 12, dy + 18, dw - 24, message, 0xFFD0D0, box_bg);

    pu_button_t left, right;
    pt_confirm_buttons(st, "Cancel", right_label, &left, &right);
    left.x += cx; left.y += cy;
    right.x += cx; right.y += cy;
    pu_button_draw(s, &left, true, SDL_MapRGB(s->format, 60, 64, 74), 0xFFFFFF);
    pu_button_draw(s, &right, true, SDL_MapRGB(s->format, 140, 60, 60), 0xFFFFFF);
}

static void putext_render(pude_window_t *win, void *state, SDL_Surface *s, int cx, int cy, int cw, int ch)
{
    (void)win;
    putext_state_t *st = state;

    Uint32 bg = SDL_MapRGB(s->format, 18, 20, 26);
    Uint32 toolbar_bg = SDL_MapRGB(s->format, 35, 38, 46);
    Uint32 status_bg = SDL_MapRGB(s->format, 20, 22, 27);
    Uint32 sel_bg = SDL_MapRGB(s->format, 50, 70, 110);
    Uint32 text_col = 0xE8E8E8;

    pu_fill_rect(s, cx, cy, cw, ch, bg);

    /* Toolbar */
    pu_fill_rect(s, cx, cy, cw, PT_TOOLBAR_H, toolbar_bg);
    pu_button_t buttons[4];
    pt_toolbar_buttons(buttons);
    for (int i = 0; i < 4; i++) {
        pu_button_t b = buttons[i];
        b.x += cx; b.y += cy;
        pu_button_draw(s, &b, true, SDL_MapRGB(s->format, 55, 60, 72), 0xFFFFFF);
    }

    /* Text area */
    int text_top = cy + PT_TOOLBAR_H;
    int vrows = pt_visible_rows(st);
    int vcols = pt_visible_cols(st);

    int r0 = -1, c0 = 0, r1 = -1, c1 = 0;
    if (st->has_selection) {
        pt_sel_range(st, &r0, &c0, &r1, &c1);
    }

    for (int row_i = 0; row_i < vrows; row_i++) {
        int line_idx = st->scroll_top + row_i;
        if (line_idx >= st->doc.line_count) break;
        pt_line_t *ln = &st->doc.lines[line_idx];
        int ry = text_top + row_i * FONT_CELL_H;

        int start_col = st->scroll_col;
        int end_col = start_col + vcols;
        if (end_col > ln->len) end_col = ln->len;

        bool line_selected = st->has_selection && line_idx >= r0 && line_idx <= r1;
        int sel_lo = line_selected ? ((line_idx == r0) ? c0 : 0) : 0;
        int sel_hi = line_selected ? ((line_idx == r1) ? c1 : ln->len) : 0;

        int dxp = cx + PT_LEFT_MARGIN;
        for (int col = start_col; col < end_col; col++) {
            bool sel = line_selected && col >= sel_lo && col < sel_hi;
            pu_draw_glyph(s, dxp, ry, ln->buf[col], text_col, sel ? sel_bg : bg);
            dxp += FONT_CELL_W;
        }

        if (line_selected && line_idx < r1 && end_col == ln->len && dxp < cx + cw) {
            int hw = FONT_CELL_W;
            if (dxp + hw > cx + cw) hw = cx + cw - dxp;
            pu_fill_rect(s, dxp, ry, hw, FONT_CELL_H, sel_bg);
        }
    }

    /* Caret -- solid, not blinking (see file header comment). */
    if (st->cursor_row >= st->scroll_top && st->cursor_row < st->scroll_top + vrows &&
        st->cursor_col >= st->scroll_col && st->cursor_col < st->scroll_col + vcols) {
        int crow = st->cursor_row - st->scroll_top;
        int ccol = st->cursor_col - st->scroll_col;
        int caret_x = cx + PT_LEFT_MARGIN + ccol * FONT_CELL_W;
        int caret_y = text_top + crow * FONT_CELL_H;
        pu_fill_rect(s, caret_x, caret_y, 2, FONT_CELL_H, 0xFFFFFF);
    }

    /* Status bar */
    int status_y = cy + ch - PT_STATUS_H;
    pu_fill_rect(s, cx, status_y, cw, PT_STATUS_H, status_bg);
    int sty = status_y + (PT_STATUS_H - FONT_CELL_H) / 2;

    const char *fname = "Untitled";
    if (st->has_filepath) {
        const char *base = strrchr(st->filepath, '/');
        fname = base ? base + 1 : st->filepath;
    }
    char namebuf[80];
    snprintf(namebuf, sizeof(namebuf), "%s%s", fname, st->modified ? " *" : "");

    char linecol[64];
    snprintf(linecol, sizeof(linecol), "Ln %d, Col %d", st->cursor_row + 1, st->cursor_col + 1);
    int rw = (int)strlen(linecol) * FONT_CELL_W;

    pu_draw_string_clipped(s, cx + 4, sty, 200, namebuf, 0xE8E8E8, status_bg);
    if (st->status_msg[0]) {
        Uint32 mcol = st->status_is_error ? 0xFF6060 : 0x90D890;
        int mx = cx + 4 + 204;
        int mw = cx + cw - mx - rw - 12;
        if (mw > 0) {
            pu_draw_string_clipped(s, mx, sty, mw, st->status_msg, mcol, status_bg);
        }
    }
    pu_draw_string_clipped(s, cx + cw - rw - 4, sty, rw + 4, linecol, 0xAAAAAA, status_bg);

    /* Modals */
    if (st->mode == PT_MODE_FILEPICKER) {
        int dx, dy, dw, dh;
        pt_modal_rect(st, &dx, &dy, &dw, &dh);
        pu_filepicker_draw(s, dx + cx, dy + cy, dw, dh, &st->picker);
    } else if (st->mode == PT_MODE_CONFIRM_DISCARD) {
        pt_draw_confirm(s, st, cx, cy, "Discard unsaved changes?", "Discard");
    } else if (st->mode == PT_MODE_CONFIRM_CLOSE) {
        pt_draw_confirm(s, st, cx, cy, "Discard unsaved changes and close?", "Discard && Close");
    }
}

/* ============================================================================
 * app_class_t callbacks.
 * ========================================================================= */

static void *putext_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    putext_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    pt_doc_init_empty(&st->doc);
    st->cw = client_w;
    st->ch = client_h;
    st->mode = PT_MODE_EDIT;
    pt_status_info(st, "New document");

    if (g_startup_path[0]) {
        char path[PT_PATH_MAX];
        strncpy(path, g_startup_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        g_startup_path[0] = '\0';

        char err[128];
        pt_doc_t newdoc;
        if (pt_doc_load(&newdoc, path, err, sizeof(err))) {
            pt_doc_free(&st->doc);
            st->doc = newdoc;
            strncpy(st->filepath, path, sizeof(st->filepath) - 1);
            st->filepath[sizeof(st->filepath) - 1] = '\0';
            st->has_filepath = true;
            char msg[192];
            snprintf(msg, sizeof(msg), "opened %s", path);
            pt_status_info(st, msg);
        } else {
            char msg[192];
            snprintf(msg, sizeof(msg), "could not open %s: %s", path, err);
            pt_status_error(st, msg);
        }
    }
    return st;
}

static void putext_destroy(pude_window_t *win, void *state)
{
    (void)win;
    putext_state_t *st = state;
    pt_doc_free(&st->doc);
    free(st);
}

static void putext_on_mouse_down(pude_window_t *win, void *state, int x, int y)
{
    putext_state_t *st = state;

    if (st->mode == PT_MODE_FILEPICKER) {
        int dx, dy, dw, dh;
        pt_modal_rect(st, &dx, &dy, &dw, &dh);
        pu_fp_result_t r = pu_filepicker_on_mouse_down(&st->picker, dx, dy, dw, dh, x, y);
        if (r == PU_FP_CANCELLED) {
            st->mode = PT_MODE_EDIT;
        } else if (r == PU_FP_CONFIRMED) {
            pt_filepicker_confirm(st);
        }
        return;
    }
    if (st->mode == PT_MODE_CONFIRM_DISCARD) {
        pu_button_t cancel_b, discard_b;
        pt_confirm_buttons(st, "Cancel", "Discard", &cancel_b, &discard_b);
        if (pu_button_hit(&cancel_b, x, y)) {
            st->mode = PT_MODE_EDIT;
            st->pending_action = PT_PENDING_NONE;
        } else if (pu_button_hit(&discard_b, x, y)) {
            pt_discard_confirmed(st);
        }
        return;
    }
    if (st->mode == PT_MODE_CONFIRM_CLOSE) {
        pu_button_t cancel_b, discard_b;
        pt_confirm_buttons(st, "Cancel", "Discard && Close", &cancel_b, &discard_b);
        if (pu_button_hit(&cancel_b, x, y)) {
            st->mode = PT_MODE_EDIT;
        } else if (pu_button_hit(&discard_b, x, y)) {
            win->self_close_request = true;
        }
        return;
    }

    if (y < PT_TOOLBAR_H) {
        pu_button_t buttons[4];
        pt_toolbar_buttons(buttons);
        for (int i = 0; i < 4; i++) {
            if (pu_button_hit(&buttons[i], x, y)) {
                pt_toolbar_action(st, i);
                return;
            }
        }
        return;
    }
    if (y >= st->ch - PT_STATUS_H) {
        return; /* status bar: no interaction */
    }

    int row, col;
    pt_row_col_at(st, x, y, &row, &col);
    st->cursor_row = row;
    st->cursor_col = col;
    st->desired_col = col;
    st->has_selection = false;
    st->sel_anchor_row = row;
    st->sel_anchor_col = col;
    st->mouse_selecting = true;
    pt_ensure_visible(st);
}

static void putext_on_mouse_move(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    putext_state_t *st = state;
    if (st->mode != PT_MODE_EDIT || !st->mouse_selecting) {
        return;
    }
    int row, col;
    pt_row_col_at(st, x, y, &row, &col);
    st->cursor_row = row;
    st->cursor_col = col;
    st->has_selection = (row != st->sel_anchor_row || col != st->sel_anchor_col);
    pt_ensure_visible(st);
}

static void putext_on_mouse_up(pude_window_t *win, void *state, int x, int y)
{
    (void)win; (void)x; (void)y;
    putext_state_t *st = state;
    st->mouse_selecting = false;
}

static void putext_on_key(pude_window_t *win, void *state, SDL_Scancode sc, key_mods_t mods, bool down)
{
    if (!down) {
        return;
    }
    putext_state_t *st = state;

    if (st->mode == PT_MODE_FILEPICKER) {
        int dx, dy, dw, dh;
        pt_modal_rect(st, &dx, &dy, &dw, &dh);
        (void)dx; (void)dy;
        pu_fp_result_t r = pu_filepicker_on_key(&st->picker, dw, dh, sc, mods);
        if (r == PU_FP_CANCELLED) {
            st->mode = PT_MODE_EDIT;
        } else if (r == PU_FP_CONFIRMED) {
            pt_filepicker_confirm(st);
        }
        return;
    }
    if (st->mode == PT_MODE_CONFIRM_DISCARD) {
        if (sc == SDL_SCANCODE_ESCAPE) {
            st->mode = PT_MODE_EDIT;
            st->pending_action = PT_PENDING_NONE;
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            pt_discard_confirmed(st);
        }
        return;
    }
    if (st->mode == PT_MODE_CONFIRM_CLOSE) {
        if (sc == SDL_SCANCODE_ESCAPE) {
            st->mode = PT_MODE_EDIT;
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            win->self_close_request = true;
        }
        return;
    }

    if (mods.ctrl) {
        switch (sc) {
        case SDL_SCANCODE_N: pt_action_new(st); return;
        case SDL_SCANCODE_O: pt_action_open(st); return;
        case SDL_SCANCODE_S: if (mods.shift) pt_action_save_as(st); else pt_action_save(st); return;
        case SDL_SCANCODE_A: pt_select_all(st); return;
        case SDL_SCANCODE_C: pt_copy(st); return;
        case SDL_SCANCODE_X: pt_cut(st); return;
        case SDL_SCANCODE_V: pt_paste(st); return;
        default: return; /* swallow other Ctrl combos so they don't leak stray characters */
        }
    }

    switch (sc) {
    case SDL_SCANCODE_LEFT: pt_move_left(st, mods.shift); return;
    case SDL_SCANCODE_RIGHT: pt_move_right(st, mods.shift); return;
    case SDL_SCANCODE_UP: pt_move_vert(st, -1, mods.shift); return;
    case SDL_SCANCODE_DOWN: pt_move_vert(st, 1, mods.shift); return;
    case SDL_SCANCODE_HOME: pt_move_home(st, mods.shift); return;
    case SDL_SCANCODE_END: pt_move_end(st, mods.shift); return;
    case SDL_SCANCODE_PAGEUP: pt_move_page(st, -1, mods.shift); return;
    case SDL_SCANCODE_PAGEDOWN: pt_move_page(st, 1, mods.shift); return;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        pt_insert_newline(st);
        pt_ensure_visible(st);
        return;
    case SDL_SCANCODE_BACKSPACE:
        pt_backspace(st);
        pt_ensure_visible(st);
        return;
    case SDL_SCANCODE_DELETE:
        pt_delete_forward(st);
        pt_ensure_visible(st);
        return;
    case SDL_SCANCODE_TAB:
        pt_insert_text(st, "    ", 4);
        pt_ensure_visible(st);
        return;
    default:
        break;
    }

    char c = pu_scancode_to_ascii(sc, mods);
    if (c) {
        pt_insert_char(st, c);
        pt_ensure_visible(st);
    }
}

static void putext_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    putext_state_t *st = state;
    st->cw = new_client_w;
    st->ch = new_client_h;
    pt_clamp_cursor(st);
    pt_ensure_visible(st);
}

static bool putext_confirm_close(pude_window_t *win, void *state)
{
    (void)win;
    putext_state_t *st = state;
    if (!st->modified) {
        return true;
    }
    st->mode = PT_MODE_CONFIRM_CLOSE;
    return false;
}

const app_class_t putext_app_class = {
    .name = "PUText",
    .default_client_w = 560,
    .default_client_h = 420,
    .min_client_w = 320,
    .min_client_h = 200,
    .create = putext_create,
    .destroy = putext_destroy,
    .render = putext_render,
    .on_key = putext_on_key,
    .on_mouse_down = putext_on_mouse_down,
    .on_mouse_up = putext_on_mouse_up,
    .on_mouse_move = putext_on_mouse_move,
    .on_resize = putext_on_resize,
    .poll = NULL,
    .is_alive = NULL,
    .confirm_close = putext_confirm_close,
};
