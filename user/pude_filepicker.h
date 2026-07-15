#ifndef PUDE_FILEPICKER_H
#define PUDE_FILEPICKER_H

/* A small, reusable graphical file-picker widget for any `pude` app that
 * needs a real Open or Save-As flow -- built while adding PUText
 * (docs/pude.md), which needed exactly this and found nothing reusable:
 * PUFiles' own browsing logic (user/pude_files.c) is a whole app_class_t,
 * not something another app can embed, and its pu_textinput_t (pude_
 * widgets.h) has no directory-listing/navigation of its own.
 *
 * Deliberately NOT a new window or a new app_class_t -- a caller embeds a
 * `pu_filepicker_t` directly in its own state struct and draws/hit-tests
 * it as a centered modal over its own client area, the same convention
 * PUFiles' own New Folder/Rename/Delete dialogs use (see pf_dialog_rect()
 * in user/pude_files.c) taken one step further into a real directory
 * browser. Every directory listing goes through ordinary opendir()/
 * readdir()/stat() against PureUNIX's real VFS, exactly like PUFiles --
 * there is no mock filesystem here either.
 *
 * Header-only (`static inline`), same convention as pude_gfx.h/pude_
 * widgets.h, so embedding this in a second app later never needs a new
 * Makefile object rule.
 */
#include "pude_app.h"
#include "pude_gfx.h"
#include "pude_widgets.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PU_FP_MAX_ENTRIES 512
#define PU_FP_NAME_MAX 64   /* matches PUREUNIX_MAX_NAME */
#define PU_FP_PATH_MAX 256  /* matches PUREUNIX_MAX_PATH */
#define PU_FP_ROW_H (FONT_CELL_H + 4)

typedef struct {
    char name[PU_FP_NAME_MAX];
    bool is_dir;
} pu_fp_entry_t;

typedef enum { PU_FP_MODE_OPEN, PU_FP_MODE_SAVE } pu_fp_mode_t;
typedef enum { PU_FP_NONE, PU_FP_CONFIRMED, PU_FP_CANCELLED } pu_fp_result_t;

typedef struct {
    pu_fp_mode_t mode;
    char cwd[PU_FP_PATH_MAX];
    pu_fp_entry_t entries[PU_FP_MAX_ENTRIES];
    int entry_count;
    int selected; /* -1 = none */
    int scroll_offset;

    pu_textinput_t filename; /* SAVE mode only */

    char status_msg[128];
    bool status_is_error;

    unsigned int last_click_time_ms;
    int last_click_index;
} pu_filepicker_t;

/* ---- path helpers (identical convention to pude_files.c's pf_join/
 * pf_parent_dir -- duplicated rather than shared because pude_files.c is
 * a whole app_class_t translation unit, not a header everyone can pull
 * in without dragging PUFiles-specific symbols along). ---- */
static inline bool pu_fp_join(char *out, size_t cap, const char *dir, const char *name)
{
    int n;
    if (strcmp(dir, "/") == 0) {
        n = snprintf(out, cap, "/%s", name);
    } else {
        n = snprintf(out, cap, "%s/%s", dir, name);
    }
    return n > 0 && (size_t)n < cap;
}

static inline void pu_fp_parent_dir(char *out, size_t cap, const char *dir)
{
    if (strcmp(dir, "/") == 0) {
        strncpy(out, "/", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    char tmp[PU_FP_PATH_MAX];
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

static inline int pu_fp_compare(const void *a, const void *b)
{
    const pu_fp_entry_t *ea = a, *eb = b;
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

static inline void pu_filepicker_load_dir(pu_filepicker_t *fp)
{
    fp->entry_count = 0;
    DIR *d = opendir(fp->cwd);
    if (!d) {
        snprintf(fp->status_msg, sizeof(fp->status_msg), "opendir: %s", strerror(errno));
        fp->status_is_error = true;
        return;
    }
    struct dirent *e;
    while (fp->entry_count < PU_FP_MAX_ENTRIES && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0) {
            continue;
        }
        if (strcmp(e->d_name, "..") == 0 && strcmp(fp->cwd, "/") == 0) {
            continue;
        }
        pu_fp_entry_t *pe = &fp->entries[fp->entry_count];
        memset(pe, 0, sizeof(*pe));
        strncpy(pe->name, e->d_name, PU_FP_NAME_MAX - 1);
        pe->is_dir = (e->d_type == DT_DIR);
        if (e->d_type == DT_LNK && strcmp(e->d_name, "..") != 0) {
            char full[PU_FP_PATH_MAX];
            struct stat sbuf;
            if (pu_fp_join(full, sizeof(full), fp->cwd, e->d_name) && stat(full, &sbuf) == 0) {
                pe->is_dir = S_ISDIR(sbuf.st_mode);
            }
        }
        fp->entry_count++;
    }
    closedir(d);
    qsort(fp->entries, (size_t)fp->entry_count, sizeof(pu_fp_entry_t), pu_fp_compare);
    fp->selected = -1;
    fp->scroll_offset = 0;
    fp->status_msg[0] = '\0';
    fp->status_is_error = false;
}

static inline void pu_filepicker_navigate(pu_filepicker_t *fp, const char *new_dir)
{
    strncpy(fp->cwd, new_dir, sizeof(fp->cwd) - 1);
    fp->cwd[sizeof(fp->cwd) - 1] = '\0';
    pu_filepicker_load_dir(fp);
}

static inline void pu_filepicker_init_common(pu_filepicker_t *fp, const char *start_dir)
{
    memset(fp, 0, sizeof(*fp));
    fp->selected = -1;
    fp->last_click_index = -1;
    strncpy(fp->cwd, (start_dir && start_dir[0]) ? start_dir : "/", sizeof(fp->cwd) - 1);
    fp->cwd[sizeof(fp->cwd) - 1] = '\0';
    pu_filepicker_load_dir(fp);
}

static inline void pu_filepicker_open_init(pu_filepicker_t *fp, const char *start_dir)
{
    pu_filepicker_init_common(fp, start_dir);
    fp->mode = PU_FP_MODE_OPEN;
}

static inline void pu_filepicker_save_init(pu_filepicker_t *fp, const char *start_dir,
                                            const char *initial_name)
{
    pu_filepicker_init_common(fp, start_dir);
    fp->mode = PU_FP_MODE_SAVE;
    pu_textinput_set(&fp->filename, initial_name);
}

/* ---- layout -- caller supplies the modal's own rect (dx,dy,dw,dh); this
 * widget never assumes it owns a whole window, only a rect within one
 * (PUText draws it centered over its own client area, same convention as
 * PUFiles' own dialogs). ---- */
typedef struct {
    pu_button_t up_btn, cancel_btn, confirm_btn;
    int path_y, path_h;
    int list_y, list_h;
    int filename_y, filename_h; /* SAVE mode only, filename_h == 0 otherwise */
    int status_y, status_h;
} pu_fp_layout_t;

static inline void pu_filepicker_layout(const pu_filepicker_t *fp, int dx, int dy, int dw, int dh,
                                         pu_fp_layout_t *lo)
{
    (void)dx;
    int y = dy;
    lo->path_y = y;
    lo->path_h = 22;
    lo->up_btn.x = dx + dw - 50;
    lo->up_btn.y = y;
    lo->up_btn.w = 46;
    lo->up_btn.h = lo->path_h - 2;
    lo->up_btn.label = "Up";
    y += lo->path_h;

    lo->filename_h = (fp->mode == PU_FP_MODE_SAVE) ? 28 : 0;
    lo->status_h = 18;
    int buttons_h = 34;

    lo->list_y = y;
    lo->list_h = dh - lo->path_h - lo->filename_h - lo->status_h - buttons_h;
    if (lo->list_h < 0) lo->list_h = 0;
    y += lo->list_h;

    if (lo->filename_h) {
        lo->filename_y = y;
        y += lo->filename_h;
    } else {
        lo->filename_y = y;
    }

    lo->status_y = y;
    y += lo->status_h;

    int bw = 92, bh = 26, gap = 12;
    int bx0 = dx + dw - 2 * bw - gap - 10;
    lo->cancel_btn.x = bx0;
    lo->cancel_btn.y = y + (buttons_h - bh) / 2;
    lo->cancel_btn.w = bw;
    lo->cancel_btn.h = bh;
    lo->cancel_btn.label = "Cancel";
    lo->confirm_btn.x = bx0 + bw + gap;
    lo->confirm_btn.y = y + (buttons_h - bh) / 2;
    lo->confirm_btn.w = bw;
    lo->confirm_btn.h = bh;
    lo->confirm_btn.label = fp->mode == PU_FP_MODE_SAVE ? "Save" : "Open";
}

static inline bool pu_filepicker_can_confirm(const pu_filepicker_t *fp)
{
    if (fp->mode == PU_FP_MODE_SAVE) {
        return fp->filename.len > 0;
    }
    return fp->selected >= 0 && fp->selected < fp->entry_count &&
           !fp->entries[fp->selected].is_dir;
}

/* Fills `out` with the file the user picked -- cwd+selected entry (OPEN)
 * or cwd+typed filename (SAVE). Only meaningful after PU_FP_CONFIRMED. */
static inline bool pu_filepicker_result_path(const pu_filepicker_t *fp, char *out, size_t cap)
{
    if (fp->mode == PU_FP_MODE_SAVE) {
        return pu_fp_join(out, cap, fp->cwd, fp->filename.buf);
    }
    if (fp->selected < 0 || fp->selected >= fp->entry_count) {
        return false;
    }
    return pu_fp_join(out, cap, fp->cwd, fp->entries[fp->selected].name);
}

/* Opens the currently-selected entry: navigates into a directory, or (SAVE
 * mode only) copies a clicked file's name into the filename field. Returns
 * true if the OPEN-mode selection should be treated as confirmed (double-
 * click/Enter on a real file). */
static inline bool pu_filepicker_activate_selected(pu_filepicker_t *fp)
{
    if (fp->selected < 0 || fp->selected >= fp->entry_count) {
        return false;
    }
    pu_fp_entry_t *e = &fp->entries[fp->selected];
    if (e->is_dir) {
        char newdir[PU_FP_PATH_MAX];
        if (strcmp(e->name, "..") == 0) {
            pu_fp_parent_dir(newdir, sizeof(newdir), fp->cwd);
        } else if (!pu_fp_join(newdir, sizeof(newdir), fp->cwd, e->name)) {
            fp->status_is_error = true;
            snprintf(fp->status_msg, sizeof(fp->status_msg), "path too long");
            return false;
        }
        pu_filepicker_navigate(fp, newdir);
        return false;
    }
    if (fp->mode == PU_FP_MODE_SAVE) {
        pu_textinput_set(&fp->filename, e->name);
        return false;
    }
    return true; /* OPEN mode: a real file was activated */
}

static inline pu_fp_result_t pu_filepicker_on_mouse_down(pu_filepicker_t *fp, int dx, int dy,
                                                          int dw, int dh, int mx, int my)
{
    pu_fp_layout_t lo;
    pu_filepicker_layout(fp, dx, dy, dw, dh, &lo);

    if (pu_button_hit(&lo.cancel_btn, mx, my)) {
        return PU_FP_CANCELLED;
    }
    if (pu_button_hit(&lo.confirm_btn, mx, my)) {
        return pu_filepicker_can_confirm(fp) ? PU_FP_CONFIRMED : PU_FP_NONE;
    }
    if (pu_button_hit(&lo.up_btn, mx, my)) {
        if (strcmp(fp->cwd, "/") != 0) {
            char parent[PU_FP_PATH_MAX];
            pu_fp_parent_dir(parent, sizeof(parent), fp->cwd);
            pu_filepicker_navigate(fp, parent);
        }
        return PU_FP_NONE;
    }
    if (my >= lo.list_y && my < lo.list_y + lo.list_h) {
        int idx = pu_list_row_at(my - lo.list_y, PU_FP_ROW_H, fp->scroll_offset, fp->entry_count);
        if (idx >= 0) {
            unsigned int now = SDL_GetTicks();
            bool double_click = (idx == fp->last_click_index) &&
                                 (now - fp->last_click_time_ms < 450);
            fp->selected = idx;
            fp->last_click_index = idx;
            fp->last_click_time_ms = now;
            if (double_click && pu_filepicker_activate_selected(fp) &&
                fp->mode == PU_FP_MODE_OPEN) {
                return PU_FP_CONFIRMED;
            }
        }
    }
    (void)mx;
    return PU_FP_NONE;
}

static inline pu_fp_result_t pu_filepicker_on_key(pu_filepicker_t *fp, int dw, int dh,
                                                   SDL_Scancode sc, key_mods_t mods)
{
    pu_fp_layout_t lo;
    pu_filepicker_layout(fp, 0, 0, dw, dh, &lo);
    int visible = pu_list_visible_rows(lo.list_h, PU_FP_ROW_H);

    if (fp->mode == PU_FP_MODE_SAVE) {
        if (sc == SDL_SCANCODE_BACKSPACE) {
            pu_textinput_backspace(&fp->filename);
            return PU_FP_NONE;
        }
        char c = pu_scancode_to_ascii(sc, mods);
        if (c && c != '/') {
            pu_textinput_putc(&fp->filename, c);
            return PU_FP_NONE;
        }
    }

    switch (sc) {
    case SDL_SCANCODE_ESCAPE:
        return PU_FP_CANCELLED;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        if (fp->mode == PU_FP_MODE_OPEN && fp->selected >= 0) {
            if (pu_filepicker_activate_selected(fp)) {
                return PU_FP_CONFIRMED;
            }
            return PU_FP_NONE;
        }
        return pu_filepicker_can_confirm(fp) ? PU_FP_CONFIRMED : PU_FP_NONE;
    case SDL_SCANCODE_UP:
        if (fp->selected > 0) {
            fp->selected--;
        } else if (fp->selected < 0 && fp->entry_count > 0) {
            fp->selected = 0;
        }
        break;
    case SDL_SCANCODE_DOWN:
        if (fp->selected + 1 < fp->entry_count) {
            fp->selected++;
        }
        break;
    case SDL_SCANCODE_PAGEUP:
        fp->selected -= visible;
        if (fp->selected < 0) fp->selected = 0;
        break;
    case SDL_SCANCODE_PAGEDOWN:
        fp->selected += visible;
        if (fp->selected >= fp->entry_count) fp->selected = fp->entry_count - 1;
        break;
    case SDL_SCANCODE_HOME:
        if (fp->entry_count > 0) fp->selected = 0;
        break;
    case SDL_SCANCODE_END:
        if (fp->entry_count > 0) fp->selected = fp->entry_count - 1;
        break;
    case SDL_SCANCODE_BACKSPACE:
        if (strcmp(fp->cwd, "/") != 0) {
            char parent[PU_FP_PATH_MAX];
            pu_fp_parent_dir(parent, sizeof(parent), fp->cwd);
            pu_filepicker_navigate(fp, parent);
        }
        break;
    default:
        break;
    }

    if (fp->selected >= 0) {
        if (fp->selected < fp->scroll_offset) fp->scroll_offset = fp->selected;
        if (visible > 0 && fp->selected >= fp->scroll_offset + visible) {
            fp->scroll_offset = fp->selected - visible + 1;
        }
    }
    return PU_FP_NONE;
}

static inline void pu_filepicker_draw(SDL_Surface *s, int dx, int dy, int dw, int dh,
                                       const pu_filepicker_t *fp)
{
    Uint32 box_bg = SDL_MapRGB(s->format, 32, 35, 44);
    Uint32 border = SDL_MapRGB(s->format, 170, 175, 185);
    Uint32 row_bg = SDL_MapRGB(s->format, 26, 28, 35);
    Uint32 row_sel_bg = SDL_MapRGB(s->format, 50, 70, 110);

    pu_fill_rect(s, dx, dy, dw, dh, box_bg);
    pu_draw_rect_outline(s, dx, dy, dw, dh, 2, border);

    pu_fp_layout_t lo;
    pu_filepicker_layout(fp, dx, dy, dw, dh, &lo);

    pu_draw_string_clipped(s, dx + 6, lo.path_y + (lo.path_h - FONT_CELL_H) / 2,
                            lo.up_btn.x - dx - 12, fp->cwd, 0xE8E8E8, box_bg);
    pu_button_draw(s, &lo.up_btn, strcmp(fp->cwd, "/") != 0,
                   SDL_MapRGB(s->format, 55, 60, 72), 0xFFFFFF);

    pu_fill_rect(s, dx + 2, lo.list_y, dw - 4, lo.list_h, row_bg);
    int visible = pu_list_visible_rows(lo.list_h, PU_FP_ROW_H);
    for (int row = 0; row < visible; row++) {
        int idx = fp->scroll_offset + row;
        if (idx >= fp->entry_count) break;
        const pu_fp_entry_t *e = &fp->entries[idx];
        int ry = lo.list_y + row * PU_FP_ROW_H;
        bool sel = (idx == fp->selected);
        Uint32 this_bg = sel ? row_sel_bg : row_bg;
        pu_fill_rect(s, dx + 2, ry, dw - 4, PU_FP_ROW_H, this_bg);
        const char *marker = e->is_dir ? "[D]" : "[F]";
        Uint32 marker_col = e->is_dir ? 0x66CCFF : 0xCCCCCC;
        pu_draw_string(s, dx + 6, ry + 2, marker, marker_col, this_bg);
        pu_draw_string_clipped(s, dx + 6 + 4 * FONT_CELL_W, ry + 2, dw - 12 - 4 * FONT_CELL_W,
                                e->name, 0xFFFFFF, this_bg);
    }

    if (lo.filename_h) {
        pu_draw_string(s, dx + 6, lo.filename_y + (lo.filename_h - FONT_CELL_H) / 2,
                        "Name:", 0xE8E8E8, box_bg);
        int nx = dx + 6 + 6 * FONT_CELL_W;
        pu_textinput_draw(s, nx, lo.filename_y + 1, dx + dw - nx - 6, lo.filename_h - 4,
                           &fp->filename);
    }

    if (fp->status_msg[0]) {
        Uint32 status_col = fp->status_is_error ? 0xFF6060 : 0x90D890;
        pu_draw_string_clipped(s, dx + 6, lo.status_y + (lo.status_h - FONT_CELL_H) / 2,
                                dw - 12, fp->status_msg, status_col, box_bg);
    }

    pu_button_draw(s, &lo.cancel_btn, true, SDL_MapRGB(s->format, 60, 64, 74), 0xFFFFFF);
    pu_button_draw(s, &lo.confirm_btn, pu_filepicker_can_confirm(fp),
                   SDL_MapRGB(s->format, 50, 110, 70), 0xFFFFFF);
}

#endif
