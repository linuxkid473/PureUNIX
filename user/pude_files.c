/* user/pude_files.c -- PUFiles, a real ring-3 graphical file manager for
 * `pude` (see user/pude_files.h and docs/pude.md's "PUFiles" section for
 * the full design writeup).
 *
 * Every directory listing/navigation/create/rename/delete below goes
 * through ordinary newlib calls (opendir/readdir/closedir/stat/mkdir/
 * rmdir/unlink/rename, user/newlib_syscalls.c) against PureUNIX's real
 * VFS/EXT2/FAT16 -- there is no mock filesystem, hardcoded listing, or
 * kernel-side GUI logic anywhere in this file. Plugs into the WM through
 * the same app_class_t (user/pude_app.h) PUTerm and Calculator use.
 */
#include "pude_files.h"
#include "pude_gfx.h"
#include "pude_widgets.h"
#include "pude_launch.h"
#include "pude_spawn.h"
#include "pude_text.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PF_MAX_ENTRIES 512
#define PF_NAME_MAX 64   /* matches PUREUNIX_MAX_NAME */
#define PF_PATH_MAX 256  /* matches PUREUNIX_MAX_PATH */

#define PF_TOPBAR_H  22
#define PF_TOOLBAR_H 26
#define PF_STATUS_H  20
#define PF_ROW_H     (FONT_CELL_H + 4)
#define PF_NUM_BUTTONS 5

#define PF_DIALOG_W 340
#define PF_DIALOG_H 116

enum { PF_BTN_UP, PF_BTN_NEW_FOLDER, PF_BTN_RENAME, PF_BTN_DELETE, PF_BTN_REFRESH };

typedef struct {
    char name[PF_NAME_MAX];
    bool is_dir;
    bool is_symlink;
    bool exec_bit;
    long size; /* -1 = unknown */
} pf_entry_t;

typedef enum {
    PF_MODE_BROWSE,
    PF_MODE_TEXT_INPUT,
    PF_MODE_CONFIRM_DELETE,
} pf_mode_t;

typedef enum { PF_INPUT_NONE, PF_INPUT_NEW_FOLDER, PF_INPUT_RENAME } pf_input_purpose_t;

typedef struct {
    char cwd[PF_PATH_MAX];
    pf_entry_t entries[PF_MAX_ENTRIES];
    int entry_count;
    int selected; /* -1 = none */
    int scroll_offset;

    int cw, ch; /* current client size */

    char status_msg[192];
    bool status_is_error;

    pf_mode_t mode;
    pf_input_purpose_t input_purpose;
    pu_textinput_t input;
    char pending_name[PF_NAME_MAX]; /* entry rename/delete is acting on */

    unsigned int last_click_time_ms;
    int last_click_index;
} pufiles_state_t;

/* ---- small string/path helpers ------------------------------------------- */

static void pf_status_info(pufiles_state_t *st, const char *msg)
{
    strncpy(st->status_msg, msg, sizeof(st->status_msg) - 1);
    st->status_msg[sizeof(st->status_msg) - 1] = '\0';
    st->status_is_error = false;
}

static void pf_status_error_msg(pufiles_state_t *st, const char *msg)
{
    strncpy(st->status_msg, msg, sizeof(st->status_msg) - 1);
    st->status_msg[sizeof(st->status_msg) - 1] = '\0';
    st->status_is_error = true;
}

static void pf_status_error(pufiles_state_t *st, const char *action, int err)
{
    snprintf(st->status_msg, sizeof(st->status_msg), "%s: %s", action, strerror(err));
    st->status_is_error = true;
}

/* Joins `dir` + `name` into `out`; false (out left unspecified) if the
 * result wouldn't fit, so every caller can refuse an operation instead of
 * silently truncating a path -- see the "long names/paths" requirement in
 * docs/pude.md. */
static bool pf_join(char *out, size_t cap, const char *dir, const char *name)
{
    int n;
    if (strcmp(dir, "/") == 0) {
        n = snprintf(out, cap, "/%s", name);
    } else {
        n = snprintf(out, cap, "%s/%s", dir, name);
    }
    return n > 0 && (size_t)n < cap;
}

static void pf_parent_dir(char *out, size_t cap, const char *dir)
{
    if (strcmp(dir, "/") == 0) {
        strncpy(out, "/", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    char tmp[PF_PATH_MAX];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        strncpy(out, "/", cap - 1);
        out[cap - 1] = '\0';
    } else {
        *slash = '\0';
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = '\0';
    }
}

static bool pf_has_suffix_ci(const char *name, const char *suffix)
{
    size_t nlen = strlen(name), slen = strlen(suffix);
    if (slen > nlen) {
        return false;
    }
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static void pf_format_size(long size, char *out, size_t cap)
{
    if (size < 0) {
        out[0] = '\0';
    } else if (size < 1024) {
        snprintf(out, cap, "%ldB", size);
    } else if (size < 1024L * 1024) {
        snprintf(out, cap, "%.1fK", (double)size / 1024.0);
    } else {
        snprintf(out, cap, "%.1fM", (double)size / (1024.0 * 1024.0));
    }
}

/* Directories first (".." always pinned above everything else), then
 * case-insensitive alphabetical -- makes a large real directory usable at
 * a glance instead of whatever raw order SYS_READDIR happened to report. */
static int pf_compare(const void *a, const void *b)
{
    const pf_entry_t *ea = a, *eb = b;
    bool a_dd = strcmp(ea->name, "..") == 0;
    bool b_dd = strcmp(eb->name, "..") == 0;
    if (a_dd != b_dd) {
        return a_dd ? -1 : 1;
    }
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

/* ---- layout math ---------------------------------------------------------- */

static int pf_list_area_h(const pufiles_state_t *st)
{
    int h = st->ch - PF_TOPBAR_H - PF_TOOLBAR_H - PF_STATUS_H;
    return h > 0 ? h : 0;
}

static void pf_clamp_scroll(pufiles_state_t *st)
{
    int visible = pu_list_visible_rows(pf_list_area_h(st), PF_ROW_H);
    int max_scroll = st->entry_count - visible;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (st->scroll_offset > max_scroll) st->scroll_offset = max_scroll;
    if (st->scroll_offset < 0) st->scroll_offset = 0;
}

static void pf_ensure_visible(pufiles_state_t *st)
{
    int visible = pu_list_visible_rows(pf_list_area_h(st), PF_ROW_H);
    if (visible <= 0 || st->selected < 0) {
        pf_clamp_scroll(st);
        return;
    }
    if (st->selected < st->scroll_offset) {
        st->scroll_offset = st->selected;
    }
    if (st->selected >= st->scroll_offset + visible) {
        st->scroll_offset = st->selected - visible + 1;
    }
    pf_clamp_scroll(st);
}

static void pf_toolbar_layout(const pufiles_state_t *st, pu_button_t out[PF_NUM_BUTTONS])
{
    static const char *labels[PF_NUM_BUTTONS] = { "Up", "New Folder", "Rename", "Delete", "Refresh" };
    int bw = st->cw / PF_NUM_BUTTONS;
    for (int i = 0; i < PF_NUM_BUTTONS; i++) {
        out[i].x = i * bw;
        out[i].y = PF_TOPBAR_H;
        out[i].w = (i == PF_NUM_BUTTONS - 1) ? (st->cw - i * bw) : bw;
        out[i].h = PF_TOOLBAR_H;
        out[i].label = labels[i];
    }
}

/* Client-local (0-based) dialog box rect for the New Folder/Rename/Delete
 * modal -- shared by both hit-testing (on_mouse_down) and rendering so
 * the two can never disagree. */
static void pf_dialog_rect(const pufiles_state_t *st, int *dx, int *dy, int *dw, int *dh)
{
    *dw = PF_DIALOG_W < st->cw - 8 ? PF_DIALOG_W : (st->cw - 8 > 40 ? st->cw - 8 : 40);
    *dh = PF_DIALOG_H < st->ch - 8 ? PF_DIALOG_H : (st->ch - 8 > 60 ? st->ch - 8 : 60);
    *dx = (st->cw - *dw) / 2;
    *dy = (st->ch - *dh) / 2;
}

static void pf_dialog_buttons(const pufiles_state_t *st, const char *left_label, const char *right_label,
                                pu_button_t *left, pu_button_t *right)
{
    int dx, dy, dw, dh;
    pf_dialog_rect(st, &dx, &dy, &dw, &dh);
    int bw = 92, bh = 26, gap = 16;
    int total = bw * 2 + gap;
    int bx0 = dx + (dw - total) / 2;
    int by = dy + dh - bh - 12;
    left->x = bx0; left->y = by; left->w = bw; left->h = bh; left->label = left_label;
    right->x = bx0 + bw + gap; right->y = by; right->w = bw; right->h = bh; right->label = right_label;
}

/* ---- directory loading ----------------------------------------------------- */

/* Loads st->cwd's real contents into st->entries[]/entry_count via ordinary
 * opendir()/readdir(); does not touch selection/scroll (callers decide
 * that -- see pf_reload_and_select()). On failure (a real ENOENT/EACCES/
 * ENOTDIR from a real opendir() call) entry_count is left at 0 and the
 * error is shown in the status bar, never silently swallowed. */
static void pf_load_dir(pufiles_state_t *st)
{
    st->entry_count = 0;

    DIR *d = opendir(st->cwd);
    if (!d) {
        pf_status_error(st, "opendir", errno);
        return;
    }

    struct dirent *e;
    while (st->entry_count < PF_MAX_ENTRIES && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0) {
            continue;
        }
        if (strcmp(e->d_name, "..") == 0 && strcmp(st->cwd, "/") == 0) {
            continue; /* nothing above root to navigate to */
        }

        pf_entry_t *pe = &st->entries[st->entry_count];
        memset(pe, 0, sizeof(*pe));
        strncpy(pe->name, e->d_name, PF_NAME_MAX - 1);
        pe->size = -1;
        pe->is_dir = (e->d_type == DT_DIR);
        pe->is_symlink = (e->d_type == DT_LNK);

        if (strcmp(e->d_name, "..") != 0) {
            char full[PF_PATH_MAX];
            if (pf_join(full, sizeof(full), st->cwd, e->d_name)) {
                struct stat sbuf;
                if (pe->is_symlink) {
                    /* Follow the link (real stat(), not lstat()) to decide
                     * whether it behaves like a directory for navigation
                     * purposes; a broken link just can't be stat()'d, and
                     * stays a plain, non-navigable entry -- opening it
                     * later reports that same real failure. */
                    if (stat(full, &sbuf) == 0) {
                        pe->is_dir = S_ISDIR(sbuf.st_mode);
                        pe->size = (long)sbuf.st_size;
                        pe->exec_bit = (sbuf.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
                    }
                } else if (!pe->is_dir) {
                    if (stat(full, &sbuf) == 0) {
                        pe->size = (long)sbuf.st_size;
                        pe->exec_bit = (sbuf.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
                    }
                }
            }
        }
        st->entry_count++;
    }
    closedir(d);

    qsort(st->entries, (size_t)st->entry_count, sizeof(pf_entry_t), pf_compare);

    char msg[48];
    snprintf(msg, sizeof(msg), "%d item%s", st->entry_count, st->entry_count == 1 ? "" : "s");
    pf_status_info(st, msg);
}

/* Reloads the current directory, then re-selects an entry by name if it's
 * still present (NULL/not-found falls back to the first entry) -- keeps
 * the cursor roughly where the user expects after Refresh/mkdir/rename/
 * delete instead of always snapping back to the top. */
static void pf_reload_and_select(pufiles_state_t *st, const char *select_name)
{
    pf_load_dir(st);
    st->selected = -1;
    if (select_name && select_name[0]) {
        for (int i = 0; i < st->entry_count; i++) {
            if (strcmp(st->entries[i].name, select_name) == 0) {
                st->selected = i;
                break;
            }
        }
    }
    if (st->selected < 0 && st->entry_count > 0) {
        st->selected = 0;
    }
    st->scroll_offset = 0;
    pf_ensure_visible(st);
}

static void pf_navigate(pufiles_state_t *st, const char *new_cwd)
{
    strncpy(st->cwd, new_cwd, sizeof(st->cwd) - 1);
    st->cwd[sizeof(st->cwd) - 1] = '\0';
    pf_reload_and_select(st, NULL);
}

static void pf_refresh(pufiles_state_t *st)
{
    char keep[PF_NAME_MAX] = "";
    if (st->selected >= 0 && st->selected < st->entry_count) {
        strncpy(keep, st->entries[st->selected].name, sizeof(keep) - 1);
    }
    pf_reload_and_select(st, keep);
}

/* ---- opening files ---------------------------------------------------------
 * Small general dispatch, not per-extension special casing scattered
 * around: directories navigate in-place; .png reuses the existing native
 * imgview via pude_launch_foreground() (docs/imgview.md) -- no PNG
 * decoding duplicated here; an executable bit refuses to run the file
 * (selecting a file must never execute it); everything else is assumed to
 * be plain text and opened in a fresh PUText window via the pude_spawn.h
 * mailbox -- the desired "double-click README.txt -> PUText opens it"
 * association (docs/pude.md's "PUText" section), covering .txt/.md/.c/.h/
 * .lua/.conf/.cfg/.ini/.sh and anything else that isn't flagged
 * executable, exactly like the previous neatvi-in-PUTerm default did (this
 * is the same "not directory, not .png, not exec -> text" bucket, just
 * opened in PUText's own graphical editor now instead of a terminal). */
static void pf_open_entry(pufiles_state_t *st, int index)
{
    if (index < 0 || index >= st->entry_count) {
        return;
    }
    pf_entry_t *e = &st->entries[index];

    if (e->is_dir) {
        char newdir[PF_PATH_MAX];
        if (strcmp(e->name, "..") == 0) {
            pf_parent_dir(newdir, sizeof(newdir), st->cwd);
        } else if (!pf_join(newdir, sizeof(newdir), st->cwd, e->name)) {
            pf_status_error_msg(st, "path too long to open");
            return;
        }
        pf_navigate(st, newdir);
        return;
    }

    char full[PF_PATH_MAX];
    if (!pf_join(full, sizeof(full), st->cwd, e->name)) {
        pf_status_error_msg(st, "path too long to open");
        return;
    }

    if (pf_has_suffix_ci(e->name, ".png")) {
        char *argv[] = { (char *)"imgview", full, NULL };
        pude_launch_foreground("/bin/imgview.elf", argv);
        pf_status_info(st, "returned from imgview");
        return;
    }

    if (e->exec_bit) {
        pf_status_error_msg(st, "refusing to run an executable file -- select it and use a terminal instead");
        return;
    }

    pude_request_spawn(&putext_app_class, full);
    pf_status_info(st, "opening in PUText...");
}

/* ---- toolbar + modal actions ------------------------------------------------ */

static void pf_begin_new_folder(pufiles_state_t *st)
{
    st->mode = PF_MODE_TEXT_INPUT;
    st->input_purpose = PF_INPUT_NEW_FOLDER;
    pu_textinput_set(&st->input, "");
}

static void pf_begin_rename(pufiles_state_t *st)
{
    if (st->selected < 0 || strcmp(st->entries[st->selected].name, "..") == 0) {
        pf_status_error_msg(st, "select a file or folder to rename first");
        return;
    }
    st->mode = PF_MODE_TEXT_INPUT;
    st->input_purpose = PF_INPUT_RENAME;
    strncpy(st->pending_name, st->entries[st->selected].name, sizeof(st->pending_name) - 1);
    st->pending_name[sizeof(st->pending_name) - 1] = '\0';
    pu_textinput_set(&st->input, st->pending_name);
}

static void pf_begin_delete(pufiles_state_t *st)
{
    if (st->selected < 0 || strcmp(st->entries[st->selected].name, "..") == 0) {
        pf_status_error_msg(st, "select a file or folder to delete first");
        return;
    }
    st->mode = PF_MODE_CONFIRM_DELETE;
    strncpy(st->pending_name, st->entries[st->selected].name, sizeof(st->pending_name) - 1);
    st->pending_name[sizeof(st->pending_name) - 1] = '\0';
}

static void pf_confirm_text_input(pufiles_state_t *st)
{
    st->mode = PF_MODE_BROWSE;
    const char *name = st->input.buf;
    if (name[0] == '\0') {
        pf_status_error_msg(st, "name cannot be empty");
        return;
    }
    if (strchr(name, '/') != NULL) {
        pf_status_error_msg(st, "name cannot contain '/'");
        return;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        pf_status_error_msg(st, "invalid name");
        return;
    }

    if (st->input_purpose == PF_INPUT_NEW_FOLDER) {
        char full[PF_PATH_MAX];
        if (!pf_join(full, sizeof(full), st->cwd, name)) {
            pf_status_error_msg(st, "path too long");
            return;
        }
        if (mkdir(full, 0755) != 0) {
            pf_status_error(st, "mkdir", errno);
            return;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "created folder '%s'", name);
        pf_reload_and_select(st, name);
        pf_status_info(st, msg);
    } else if (st->input_purpose == PF_INPUT_RENAME) {
        char oldp[PF_PATH_MAX], newp[PF_PATH_MAX];
        if (!pf_join(oldp, sizeof(oldp), st->cwd, st->pending_name) ||
            !pf_join(newp, sizeof(newp), st->cwd, name)) {
            pf_status_error_msg(st, "path too long");
            return;
        }
        if (rename(oldp, newp) != 0) {
            pf_status_error(st, "rename", errno);
            return;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "renamed '%s' -> '%s'", st->pending_name, name);
        pf_reload_and_select(st, name);
        pf_status_info(st, msg);
    }
}

static void pf_confirm_delete(pufiles_state_t *st)
{
    st->mode = PF_MODE_BROWSE;
    int idx = -1;
    for (int i = 0; i < st->entry_count; i++) {
        if (strcmp(st->entries[i].name, st->pending_name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        pf_status_error_msg(st, "entry no longer exists");
        return;
    }
    pf_entry_t *e = &st->entries[idx];
    char full[PF_PATH_MAX];
    if (!pf_join(full, sizeof(full), st->cwd, e->name)) {
        pf_status_error_msg(st, "path too long");
        return;
    }

    /* rmdir() on a non-empty directory legitimately fails (ENOTEMPTY,
     * kernel/vfs.c) -- that failure is exactly what must surface here,
     * not be swallowed or treated as success. No recursive delete in
     * this first version, by design (docs/pude.md). */
    int r = e->is_dir ? rmdir(full) : unlink(full);
    if (r != 0) {
        pf_status_error(st, e->is_dir ? "rmdir" : "unlink", errno);
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "deleted '%s'", e->name);
    pf_reload_and_select(st, NULL);
    pf_status_info(st, msg);
}

static void pf_toolbar_action(pufiles_state_t *st, int idx)
{
    switch (idx) {
    case PF_BTN_UP:
        if (strcmp(st->cwd, "/") != 0) {
            char parent[PF_PATH_MAX];
            pf_parent_dir(parent, sizeof(parent), st->cwd);
            pf_navigate(st, parent);
        }
        break;
    case PF_BTN_NEW_FOLDER:
        pf_begin_new_folder(st);
        break;
    case PF_BTN_RENAME:
        pf_begin_rename(st);
        break;
    case PF_BTN_DELETE:
        pf_begin_delete(st);
        break;
    case PF_BTN_REFRESH:
        pf_refresh(st);
        break;
    default:
        break;
    }
}

/* ---- app_class_t callbacks -------------------------------------------------- */

static void *pufiles_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    pufiles_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    strcpy(st->cwd, "/");
    st->cw = client_w;
    st->ch = client_h;
    st->selected = -1;
    st->last_click_index = -1;
    st->mode = PF_MODE_BROWSE;
    pf_load_dir(st);
    st->selected = st->entry_count > 0 ? 0 : -1;
    return st;
}

static void pufiles_destroy(pude_window_t *win, void *state)
{
    (void)win;
    free(state);
}

static void pufiles_render(pude_window_t *win, void *state, SDL_Surface *s,
                            int cx, int cy, int cw, int ch)
{
    (void)win;
    pufiles_state_t *st = state;

    Uint32 bg = SDL_MapRGB(s->format, 24, 26, 32);
    Uint32 topbar_bg = SDL_MapRGB(s->format, 35, 38, 46);
    Uint32 row_bg = SDL_MapRGB(s->format, 28, 30, 38);
    Uint32 row_sel_bg = SDL_MapRGB(s->format, 50, 70, 110);
    Uint32 status_bg = SDL_MapRGB(s->format, 20, 22, 27);

    pu_fill_rect(s, cx, cy, cw, ch, bg);

    /* Path bar */
    pu_fill_rect(s, cx, cy, cw, PF_TOPBAR_H, topbar_bg);
    pu_draw_string_clipped(s, cx + 4, cy + (PF_TOPBAR_H - FONT_CELL_H) / 2, cw - 8,
                            st->cwd, 0xE8E8E8, topbar_bg);

    /* Toolbar */
    pu_button_t buttons[PF_NUM_BUTTONS];
    pf_toolbar_layout(st, buttons);
    for (int i = 0; i < PF_NUM_BUTTONS; i++) {
        pu_button_t b = buttons[i];
        b.x += cx;
        b.y += cy;
        bool enabled = true;
        if (i == PF_BTN_UP) {
            enabled = strcmp(st->cwd, "/") != 0;
        } else if (i == PF_BTN_RENAME || i == PF_BTN_DELETE) {
            enabled = st->selected >= 0 && strcmp(st->entries[st->selected].name, "..") != 0;
        }
        pu_button_draw(s, &b, enabled, SDL_MapRGB(s->format, 55, 60, 72), 0xFFFFFF);
    }

    /* List */
    int list_top = cy + PF_TOPBAR_H + PF_TOOLBAR_H;
    int list_h = pf_list_area_h(st);
    pu_fill_rect(s, cx, list_top, cw, list_h, row_bg);
    int visible = pu_list_visible_rows(list_h, PF_ROW_H);
    for (int row = 0; row < visible; row++) {
        int idx = st->scroll_offset + row;
        if (idx >= st->entry_count) {
            break;
        }
        pf_entry_t *e = &st->entries[idx];
        int ry = list_top + row * PF_ROW_H;
        bool sel = (idx == st->selected);
        Uint32 this_row_bg = sel ? row_sel_bg : row_bg;
        pu_fill_rect(s, cx, ry, cw, PF_ROW_H, this_row_bg);

        const char *marker = e->is_dir ? "[D]" : (e->is_symlink ? "[L]" : "[F]");
        Uint32 marker_col = e->is_dir ? 0x66CCFF : (e->is_symlink ? 0xFFCC66 : 0xCCCCCC);
        pu_draw_string(s, cx + 4, ry + 2, marker, marker_col, this_row_bg);

        int name_x = cx + 4 + 4 * FONT_CELL_W;
        char sizebuf[16] = "";
        int size_w = 0;
        if (!e->is_dir && e->size >= 0) {
            pf_format_size(e->size, sizebuf, sizeof(sizebuf));
            size_w = 8 * FONT_CELL_W;
        }
        int name_max_w = cx + cw - name_x - size_w - 4;
        if (name_max_w < 0) {
            name_max_w = 0;
        }
        pu_draw_string_clipped(s, name_x, ry + 2, name_max_w, e->name, 0xFFFFFF, this_row_bg);

        if (size_w > 0) {
            int sx = cx + cw - size_w - 4;
            pu_draw_string_clipped(s, sx, ry + 2, size_w, sizebuf, 0xAAAAAA, this_row_bg);
        }
    }

    /* Status bar */
    pu_fill_rect(s, cx, cy + ch - PF_STATUS_H, cw, PF_STATUS_H, status_bg);
    Uint32 status_col = st->status_is_error ? 0xFF6060 : 0x90D890;
    pu_draw_string_clipped(s, cx + 4, cy + ch - PF_STATUS_H + (PF_STATUS_H - FONT_CELL_H) / 2,
                            cw - 8, st->status_msg, status_col, status_bg);

    if (st->mode != PF_MODE_BROWSE) {
        int dx, dy, dw, dh;
        pf_dialog_rect(st, &dx, &dy, &dw, &dh);
        dx += cx;
        dy += cy;

        Uint32 box_bg = SDL_MapRGB(s->format, 45, 48, 58);
        Uint32 border = SDL_MapRGB(s->format, 170, 175, 185);
        pu_fill_rect(s, dx, dy, dw, dh, box_bg);
        pu_draw_rect_outline(s, dx, dy, dw, dh, 2, border);

        pu_button_t left, right;
        if (st->mode == PF_MODE_TEXT_INPUT) {
            const char *title = st->input_purpose == PF_INPUT_NEW_FOLDER
                                     ? "New folder name:"
                                     : "Rename to:";
            pu_draw_string_clipped(s, dx + 10, dy + 12, dw - 20, title, 0xFFFFFF, box_bg);
            pu_textinput_draw(s, dx + 10, dy + 36, dw - 20, 26, &st->input);
            pf_dialog_buttons(st, "Cancel", "OK", &left, &right);
        } else {
            char msg[160];
            snprintf(msg, sizeof(msg), "Delete '%s'? This cannot be undone.", st->pending_name);
            pu_draw_string_clipped(s, dx + 10, dy + 18, dw - 20, msg, 0xFFD0D0, box_bg);
            pf_dialog_buttons(st, "Cancel", "Delete", &left, &right);
        }
        left.x += cx; left.y += cy;
        right.x += cx; right.y += cy;
        pu_button_draw(s, &left, true, SDL_MapRGB(s->format, 60, 64, 74), 0xFFFFFF);
        pu_button_draw(s, &right, true, SDL_MapRGB(s->format, 140, 60, 60), 0xFFFFFF);
    }
}

static void pufiles_on_mouse_down(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    pufiles_state_t *st = state;

    if (st->mode == PF_MODE_TEXT_INPUT) {
        pu_button_t cancel_b, ok_b;
        pf_dialog_buttons(st, "Cancel", "OK", &cancel_b, &ok_b);
        if (pu_button_hit(&cancel_b, x, y)) {
            st->mode = PF_MODE_BROWSE;
        } else if (pu_button_hit(&ok_b, x, y)) {
            pf_confirm_text_input(st);
        }
        return;
    }
    if (st->mode == PF_MODE_CONFIRM_DELETE) {
        pu_button_t cancel_b, del_b;
        pf_dialog_buttons(st, "Cancel", "Delete", &cancel_b, &del_b);
        if (pu_button_hit(&cancel_b, x, y)) {
            st->mode = PF_MODE_BROWSE;
        } else if (pu_button_hit(&del_b, x, y)) {
            pf_confirm_delete(st);
        }
        return;
    }

    pu_button_t buttons[PF_NUM_BUTTONS];
    pf_toolbar_layout(st, buttons);
    for (int i = 0; i < PF_NUM_BUTTONS; i++) {
        if (pu_button_hit(&buttons[i], x, y)) {
            pf_toolbar_action(st, i);
            return;
        }
    }

    int list_top = PF_TOPBAR_H + PF_TOOLBAR_H;
    int list_h = pf_list_area_h(st);
    if (y >= list_top && y < list_top + list_h) {
        int idx = pu_list_row_at(y - list_top, PF_ROW_H, st->scroll_offset, st->entry_count);
        if (idx >= 0) {
            unsigned int now = SDL_GetTicks();
            bool double_click = (idx == st->last_click_index) &&
                                 (now - st->last_click_time_ms < 450);
            st->selected = idx;
            st->last_click_index = idx;
            st->last_click_time_ms = now;
            if (double_click) {
                pf_open_entry(st, idx);
            }
        }
    }
}

static void pufiles_on_key(pude_window_t *win, void *state, SDL_Scancode sc, key_mods_t mods, bool down)
{
    (void)win;
    if (!down) {
        return;
    }
    pufiles_state_t *st = state;

    if (st->mode == PF_MODE_TEXT_INPUT) {
        if (sc == SDL_SCANCODE_ESCAPE) {
            st->mode = PF_MODE_BROWSE;
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            pf_confirm_text_input(st);
        } else if (sc == SDL_SCANCODE_BACKSPACE) {
            pu_textinput_backspace(&st->input);
        } else {
            char c = pu_scancode_to_ascii(sc, mods);
            if (c) {
                pu_textinput_putc(&st->input, c);
            }
        }
        return;
    }
    if (st->mode == PF_MODE_CONFIRM_DELETE) {
        if (sc == SDL_SCANCODE_ESCAPE) {
            st->mode = PF_MODE_BROWSE;
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            pf_confirm_delete(st);
        }
        return;
    }

    int visible = pu_list_visible_rows(pf_list_area_h(st), PF_ROW_H);
    switch (sc) {
    case SDL_SCANCODE_UP:
        if (st->selected > 0) {
            st->selected--;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_DOWN:
        if (st->selected + 1 < st->entry_count) {
            st->selected++;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_PAGEUP:
        if (visible > 0 && st->entry_count > 0) {
            st->selected -= visible;
            if (st->selected < 0) st->selected = 0;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_PAGEDOWN:
        if (visible > 0 && st->entry_count > 0) {
            st->selected += visible;
            if (st->selected >= st->entry_count) st->selected = st->entry_count - 1;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_HOME:
        if (st->entry_count > 0) {
            st->selected = 0;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_END:
        if (st->entry_count > 0) {
            st->selected = st->entry_count - 1;
            pf_ensure_visible(st);
        }
        break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        if (st->selected >= 0) {
            pf_open_entry(st, st->selected);
        }
        break;
    case SDL_SCANCODE_BACKSPACE:
        if (strcmp(st->cwd, "/") != 0) {
            char parent[PF_PATH_MAX];
            pf_parent_dir(parent, sizeof(parent), st->cwd);
            pf_navigate(st, parent);
        }
        break;
    case SDL_SCANCODE_F5:
        pf_refresh(st);
        break;
    default:
        break;
    }
}

static void pufiles_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    pufiles_state_t *st = state;
    st->cw = new_client_w;
    st->ch = new_client_h;
    pf_ensure_visible(st);
}

const app_class_t pufiles_app_class = {
    .name = "PUFiles",
    .default_client_w = 480,
    .default_client_h = 360,
    .min_client_w = 320,
    .min_client_h = 200,
    .create = pufiles_create,
    .destroy = pufiles_destroy,
    .render = pufiles_render,
    .on_key = pufiles_on_key,
    .on_mouse_down = pufiles_on_mouse_down,
    .on_mouse_up = NULL,
    .on_resize = pufiles_on_resize,
    .poll = NULL,
    .is_alive = NULL,
    .icon_draw = pu_icon_pufiles,
    .graphical = true,
    .pinned_default = true,
};
