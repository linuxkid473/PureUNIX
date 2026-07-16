/* user/pude_settings.c -- Settings, a real ring-3 GUI app for `pude`
 * (docs/pude.md) exposing exactly one setting: the desktop wallpaper.
 *
 * Reuses the embedded pude_filepicker.h widget (the same OPEN-mode modal
 * PUText's own Open uses -- see pude_text.c) rather than inventing a
 * second file-browsing UI, restricted to `.png` files by a plain
 * extension check on the confirmed path (pu_filepicker_t itself has no
 * extension-filtering of its own, and doesn't need one just for this).
 * Actually applying and persisting a chosen wallpaper is entirely user/
 * pude_wallpaper.c/.h's job -- this file only drives that module's public
 * API from a button/modal.
 */
#include "pude_settings.h"
#include "pude_filepicker.h"
#include "pude_gfx.h"
#include "pude_icon.h"
#include "pude_wallpaper.h"
#include "pude_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ST_PAD    16
#define ST_BTN_W  120
#define ST_BTN_H  28

typedef enum { ST_MODE_MAIN, ST_MODE_FILEPICKER } st_mode_t;

typedef struct {
    st_mode_t mode;
    pu_filepicker_t picker;
    int cw, ch; /* last-known client size, for hit-testing/layout */
    char status_msg[160];
    bool status_is_error;
} settings_state_t;

typedef struct {
    int header_y;
    int path_y;
    pu_button_t choose_btn;
    int status_y;
} st_layout_t;

static void settings_layout(st_layout_t *lo)
{
    lo->header_y = ST_PAD;
    lo->path_y = lo->header_y + FONT_CELL_H + 10;
    lo->choose_btn.x = ST_PAD;
    lo->choose_btn.y = lo->path_y + FONT_CELL_H + 12;
    lo->choose_btn.w = ST_BTN_W;
    lo->choose_btn.h = ST_BTN_H;
    lo->choose_btn.label = "Choose...";
    lo->status_y = lo->choose_btn.y + ST_BTN_H + 12;
}

/* Centered modal rect for the embedded file picker, client-relative --
 * same technique as PUText's own pt_modal_rect() (user/pude_text.c). */
static void settings_modal_rect(const settings_state_t *st, int *dx, int *dy, int *dw, int *dh)
{
    int w = 380;
    if (w > st->cw - 16) w = (st->cw - 16 > 60) ? st->cw - 16 : 60;
    int h = 300;
    if (h > st->ch - 16) h = (st->ch - 16 > 80) ? st->ch - 16 : 80;
    *dw = w;
    *dh = h;
    *dx = (st->cw - w) / 2;
    *dy = (st->ch - h) / 2;
}

static void settings_begin_picker(settings_state_t *st)
{
    const char *current = pude_wallpaper_current_path();
    char startdir[PU_FP_PATH_MAX] = "/";
    if (current) {
        const char *slash = strrchr(current, '/');
        if (slash && slash != current) {
            size_t n = (size_t)(slash - current);
            if (n >= sizeof(startdir)) n = sizeof(startdir) - 1;
            memcpy(startdir, current, n);
            startdir[n] = '\0';
        }
    }
    pu_filepicker_open_init(&st->picker, startdir);
    st->mode = ST_MODE_FILEPICKER;
}

/* Only `.png` (case-insensitive) may be confirmed -- everything else
 * re-shows the picker's own status line with an error instead of closing
 * it, exactly like a real "Open" dialog rejecting an unsupported type. */
static bool settings_has_png_ext(const char *path)
{
    size_t len = strlen(path);
    return len > 4 && strcasecmp(path + len - 4, ".png") == 0;
}

static void settings_filepicker_confirm(settings_state_t *st)
{
    char path[PU_FP_PATH_MAX];
    if (!pu_filepicker_result_path(&st->picker, path, sizeof(path))) {
        st->picker.status_is_error = true;
        snprintf(st->picker.status_msg, sizeof(st->picker.status_msg), "path too long");
        return;
    }
    if (!settings_has_png_ext(path)) {
        st->picker.status_is_error = true;
        snprintf(st->picker.status_msg, sizeof(st->picker.status_msg),
                 "Only .png files are supported");
        return;
    }
    if (!pude_wallpaper_set(path)) {
        st->status_is_error = true;
        snprintf(st->status_msg, sizeof(st->status_msg), "failed to load '%s'", path);
    } else {
        st->status_is_error = false;
        snprintf(st->status_msg, sizeof(st->status_msg), "wallpaper set to %s", path);
    }
    st->mode = ST_MODE_MAIN;
}

/* ---- app_class_t callbacks ------------------------------------------------ */

static void *settings_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    settings_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    st->mode = ST_MODE_MAIN;
    st->cw = client_w;
    st->ch = client_h;
    return st;
}

static void settings_destroy(pude_window_t *win, void *state)
{
    (void)win;
    free(state);
}

static void settings_render(pude_window_t *win, void *state, SDL_Surface *s,
                             int cx, int cy, int cw, int ch)
{
    (void)win;
    settings_state_t *st = state;
    Uint32 bg = SDL_MapRGB(s->format, 30, 34, 44);
    pu_fill_rect(s, cx, cy, cw, ch, bg);

    st_layout_t lo;
    settings_layout(&lo);

    pu_draw_string(s, cx + ST_PAD, cy + lo.header_y, "Wallpaper", 0xFFFFFF, bg);

    const char *path = pude_wallpaper_current_path();
    char line[PUDE_WALLPAPER_PATH_MAX + 16];
    snprintf(line, sizeof(line), "Current: %s", path ? path : "(none - default background)");
    pu_draw_string_clipped(s, cx + ST_PAD, cy + lo.path_y, cw - 2 * ST_PAD, line, 0xC8CCD4, bg);

    pu_button_t btn = lo.choose_btn;
    btn.x += cx;
    btn.y += cy;
    pu_button_draw(s, &btn, true, SDL_MapRGB(s->format, 55, 60, 72), 0xFFFFFF);

    if (st->status_msg[0]) {
        Uint32 col = st->status_is_error ? 0xFF6060 : 0x90D890;
        pu_draw_string_clipped(s, cx + ST_PAD, cy + lo.status_y, cw - 2 * ST_PAD, st->status_msg,
                                col, bg);
    }

    if (st->mode == ST_MODE_FILEPICKER) {
        int dx, dy, dw, dh;
        settings_modal_rect(st, &dx, &dy, &dw, &dh);
        pu_filepicker_draw(s, dx + cx, dy + cy, dw, dh, &st->picker);
    }
}

static void settings_on_mouse_down(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    settings_state_t *st = state;

    if (st->mode == ST_MODE_FILEPICKER) {
        int dx, dy, dw, dh;
        settings_modal_rect(st, &dx, &dy, &dw, &dh);
        pu_fp_result_t r = pu_filepicker_on_mouse_down(&st->picker, dx, dy, dw, dh, x, y);
        if (r == PU_FP_CANCELLED) {
            st->mode = ST_MODE_MAIN;
        } else if (r == PU_FP_CONFIRMED) {
            settings_filepicker_confirm(st);
        }
        return;
    }

    st_layout_t lo;
    settings_layout(&lo);
    if (pu_button_hit(&lo.choose_btn, x, y)) {
        settings_begin_picker(st);
    }
}

static void settings_on_key(pude_window_t *win, void *state, SDL_Scancode sc,
                             key_mods_t mods, bool down)
{
    (void)win;
    if (!down) {
        return;
    }
    settings_state_t *st = state;
    if (st->mode != ST_MODE_FILEPICKER) {
        return;
    }
    int dx, dy, dw, dh;
    settings_modal_rect(st, &dx, &dy, &dw, &dh);
    (void)dx;
    (void)dy;
    pu_fp_result_t r = pu_filepicker_on_key(&st->picker, dw, dh, sc, mods);
    if (r == PU_FP_CANCELLED) {
        st->mode = ST_MODE_MAIN;
    } else if (r == PU_FP_CONFIRMED) {
        settings_filepicker_confirm(st);
    }
}

static void settings_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    settings_state_t *st = state;
    st->cw = new_client_w;
    st->ch = new_client_h;
}

const app_class_t settings_app_class = {
    .name = "Settings",
    .default_client_w = 420,
    .default_client_h = 260,
    .min_client_w = 320,
    .min_client_h = 200,
    .create = settings_create,
    .destroy = settings_destroy,
    .render = settings_render,
    .on_key = settings_on_key,
    .on_mouse_down = settings_on_mouse_down,
    .on_mouse_up = NULL,
    .on_resize = settings_on_resize,
    .poll = NULL,
    .is_alive = NULL,
    .icon_draw = pu_icon_settings,
    .graphical = true,
    .pinned_default = true,
};
