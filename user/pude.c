/* user/pude.c — pude: PureUNIX's graphical desktop shell.
 *
 * A real ring-3 window manager: a fullscreen SDL2 window (docs/sdl-port.md)
 * drawing a dark desktop, a top-left "Menu" control that opens an
 * application launcher, and zero or more chrome-decorated app windows
 * (title bar, close button, resize grip). Every window is backed by an
 * app_class_t (user/pude_app.h) -- a small, generic lifecycle vtable, not
 * a `pude`-specific special case per app. Two apps are registered today:
 * PUTerm (user/pude_term.h, a real terminal emulator over a real pty and
 * a forked+exec'd BusyBox ash) and Calculator (user/pude_calc.h, pure
 * ring-3 arithmetic). Neither app's own logic lives in this file; see
 * docs/pude.md for the full architecture and how to add a third app.
 *
 * This file owns exactly: the window list (geometry, z-order, focus),
 * chrome rendering and hit-testing (move/resize/close), the launcher
 * menu, and the top-level SDL event loop. It contains no terminal or
 * calculator logic whatsoever.
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pude_app.h"
#include "pude_calc.h"
#include "pude_files.h"
#include "pude_gfx.h"
#include "pude_spawn.h"
#include "pude_term.h"
#include "pude_text.h"

/* ---- Chrome layout ------------------------------------------------------- */

#define BORDER        3
#define TITLEBAR_PAD  6
#define CLOSE_BTN_W   20
#define RESIZE_GRIP   22
#define MARGIN        24
#define MAX_WINDOWS   12

#define MENU_BTN_X 8
#define MENU_BTN_Y 8
#define MENU_BTN_W 90
#define MENU_BTN_H 26
#define MENU_ITEM_W 170
#define MENU_ITEM_H 28

static int titlebar_h(void)
{
    return FONT_CELL_H + TITLEBAR_PAD * 2;
}

/* ---- Window pool + z-order ------------------------------------------------
 * `pude_window_t` instances live in a fixed pool (same style as
 * kernel/pty.c's MAX_PTYS) -- `win_order` holds pointers into that pool in
 * z-order (index 0 = bottommost, the last entry = topmost/focused). Only
 * the order array is shuffled on click-to-front/close; the pool slots
 * themselves never move, so an app's own `pude_window_t *` stays valid for
 * its whole lifetime. */
static pude_window_t window_pool[MAX_WINDOWS];
static bool pool_used[MAX_WINDOWS];
static pude_window_t *win_order[MAX_WINDOWS];
static int win_count;
static int spawn_count;

static const app_class_t *const g_apps[] = {
    &puterm_app_class,
    &calc_app_class,
    &pufiles_app_class,
    &putext_app_class,
};
#define NUM_APPS (int)(sizeof(g_apps) / sizeof(g_apps[0]))

static void window_client_rect(const pude_window_t *win, int *cx, int *cy, int *cw, int *ch)
{
    *cx = win->x + BORDER;
    *cy = win->y + BORDER + titlebar_h();
    *cw = win->w - 2 * BORDER;
    *ch = win->h - 2 * BORDER - titlebar_h();
}

static void close_button_rect(const pude_window_t *win, int *bx, int *by, int *bw, int *bh)
{
    *bw = CLOSE_BTN_W;
    *bh = titlebar_h() - 2 * (TITLEBAR_PAD / 2 + 1);
    *bx = win->x + win->w - BORDER - TITLEBAR_PAD - *bw;
    *by = win->y + BORDER + (titlebar_h() - *bh) / 2;
}

static bool point_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void close_window_at(int z_index);

/* Spawns a new window for `cls`, cascading its initial position so
 * repeated launches (e.g. opening PUTerm twice) don't stack exactly on
 * top of each other. Returns false (no-op) if the window pool is full or
 * the app's own create() fails. */
static bool spawn_window(const app_class_t *cls, int screen_w, int screen_h)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!pool_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || win_count >= MAX_WINDOWS) {
        return false;
    }

    pude_window_t *win = &window_pool[slot];
    memset(win, 0, sizeof(*win));
    win->cls = cls;
    strncpy(win->title, cls->name, sizeof(win->title) - 1);

    int whole_w = cls->default_client_w + 2 * BORDER;
    int whole_h = cls->default_client_h + 2 * BORDER + titlebar_h();
    int cascade = (spawn_count % 8) * 26;
    win->x = MARGIN + cascade;
    win->y = MARGIN + cascade;
    win->w = whole_w;
    win->h = whole_h;
    if (win->x + win->w > screen_w) win->x = screen_w - win->w;
    if (win->y + win->h > screen_h) win->y = screen_h - win->h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;

    win->state = cls->create(win, cls->default_client_w, cls->default_client_h);
    if (!win->state) {
        return false;
    }

    pool_used[slot] = true;
    win_order[win_count++] = win;
    spawn_count++;
    return true;
}

static void bring_to_front(int z_index)
{
    if (z_index == win_count - 1) {
        return;
    }
    pude_window_t *win = win_order[z_index];
    for (int i = z_index; i < win_count - 1; i++) {
        win_order[i] = win_order[i + 1];
    }
    win_order[win_count - 1] = win;
}

static void close_window_at(int z_index)
{
    pude_window_t *win = win_order[z_index];
    if (win->cls->destroy) {
        win->cls->destroy(win, win->state);
    }
    int slot = (int)(win - window_pool);
    pool_used[slot] = false;
    for (int i = z_index; i < win_count - 1; i++) {
        win_order[i] = win_order[i + 1];
    }
    win_count--;
}

/* ---- Rendering ------------------------------------------------------------ */

static void draw_window_chrome(SDL_Surface *s, const pude_window_t *win, bool focused)
{
    Uint32 border_col = focused ? SDL_MapRGB(s->format, 170, 175, 185)
                                 : SDL_MapRGB(s->format, 110, 114, 122);
    Uint32 titlebar_col = focused ? SDL_MapRGB(s->format, 40, 80, 140)
                                   : SDL_MapRGB(s->format, 55, 60, 70);
    pu_fill_rect(s, win->x, win->y, win->w, win->h, border_col);

    int tb_h = titlebar_h();
    pu_fill_rect(s, win->x + BORDER, win->y + BORDER, win->w - 2 * BORDER, tb_h, titlebar_col);
    pu_draw_string(s, win->x + BORDER + TITLEBAR_PAD, win->y + BORDER + TITLEBAR_PAD / 2,
                   win->title, 0xFFFFFF, titlebar_col);

    int bx, by, bw, bh;
    close_button_rect(win, &bx, &by, &bw, &bh);
    pu_fill_rect(s, bx, by, bw, bh, SDL_MapRGB(s->format, 170, 50, 50));
    pu_draw_string_centered(s, bx, by, bw, bh, "X", 0xFFFFFF, SDL_MapRGB(s->format, 170, 50, 50));

    int cx, cy, cw, ch;
    window_client_rect(win, &cx, &cy, &cw, &ch);
    pu_fill_rect(s, cx, cy, cw, ch, SDL_MapRGB(s->format, 0, 0, 0));
    win->cls->render((pude_window_t *)win, win->state, s, cx, cy, cw, ch);

    Uint32 grip_col = SDL_MapRGB(s->format, 120, 125, 135);
    int gx0 = win->x + win->w - RESIZE_GRIP;
    int gy0 = win->y + win->h - RESIZE_GRIP;
    for (int i = 3; i < RESIZE_GRIP; i += 3) {
        for (int k = 0; k < 2; k++) {
            pu_put_pixel(s, gx0 + i + k, gy0 + RESIZE_GRIP - 1, grip_col);
            pu_put_pixel(s, gx0 + RESIZE_GRIP - 1, gy0 + i + k, grip_col);
        }
    }
}

static void draw_menu(SDL_Surface *s, bool menu_open)
{
    pu_fill_rect(s, MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H,
                 SDL_MapRGB(s->format, 50, 55, 65));
    pu_draw_rect_outline(s, MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, 1,
                          SDL_MapRGB(s->format, 150, 155, 165));
    pu_draw_string_centered(s, MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H,
                             "Menu", 0xFFFFFF, SDL_MapRGB(s->format, 50, 55, 65));

    if (!menu_open) {
        return;
    }
    int py = MENU_BTN_Y + MENU_BTN_H + 4;
    int ph = NUM_APPS * MENU_ITEM_H + 8;
    pu_fill_rect(s, MENU_BTN_X, py, MENU_ITEM_W, ph, SDL_MapRGB(s->format, 45, 50, 60));
    pu_draw_rect_outline(s, MENU_BTN_X, py, MENU_ITEM_W, ph, 1, SDL_MapRGB(s->format, 150, 155, 165));
    for (int i = 0; i < NUM_APPS; i++) {
        int iy = py + 4 + i * MENU_ITEM_H;
        pu_draw_rect_outline(s, MENU_BTN_X + 4, iy, MENU_ITEM_W - 8, MENU_ITEM_H - 2, 1,
                              SDL_MapRGB(s->format, 90, 95, 105));
        pu_draw_string_centered(s, MENU_BTN_X + 4, iy, MENU_ITEM_W - 8, MENU_ITEM_H - 2,
                                 g_apps[i]->name, 0xFFFFFF, SDL_MapRGB(s->format, 45, 50, 60));
    }
}

/* A simple, unmistakable arrow pointer -- this environment has no host
 * compositor drawing a cursor overlay (real PS/2 mouse input, no window
 * system chrome, see docs/sdl-port.md), so without this the pointer
 * position would be entirely invisible between clicks. */
static void draw_cursor(SDL_Surface *s, int x, int y)
{
    static const int widths[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 6, 2, 2, 2, 0, 0 };
    Uint32 white = SDL_MapRGB(s->format, 255, 255, 255);
    Uint32 black = SDL_MapRGB(s->format, 0, 0, 0);
    for (int r = 0; r < 16; r++) {
        int w = widths[r];
        for (int c = 0; c <= w; c++) {
            bool edge = (c == w || c == 0 || r == 0);
            pu_put_pixel(s, x + c, y + r, edge ? black : white);
        }
    }
}

static void render_frame(SDL_Window *sdl_win, SDL_Surface *s, bool menu_open, int mouse_x, int mouse_y)
{
    SDL_Rect full = { 0, 0, s->w, s->h };
    SDL_FillRect(s, &full, SDL_MapRGB(s->format, 20, 24, 34));

    for (int i = 0; i < win_count; i++) {
        draw_window_chrome(s, win_order[i], i == win_count - 1);
    }

    draw_menu(s, menu_open);
    draw_cursor(s, mouse_x, mouse_y);
    SDL_UpdateWindowSurface(sdl_win);
}

/* ---- Keyboard modifier tracking -------------------------------------------
 * Physical Shift/Ctrl state is tracked once by the WM (not per-app) and
 * passed to whichever window is focused -- an app never needs its own
 * modifier bookkeeping. */

static bool is_modifier_scancode(SDL_Scancode sc)
{
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT ||
           sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("pude", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                        0, 0, SDL_WINDOW_FULLSCREEN);
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Surface *surface = SDL_GetWindowSurface(win);
    if (!surface || surface->format->BytesPerPixel != 4) {
        SDL_Log("pude requires a 32bpp window surface");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int screen_w = surface->w, screen_h = surface->h;

    /* The desktop starts with no windows open at all -- the launcher is
     * the only way to open one. */
    win_count = 0;
    spawn_count = 0;
    memset(pool_used, 0, sizeof(pool_used));

    key_mods_t mods = { false, false };
    bool menu_open = false;
    bool quit = false;

    int mouse_x = screen_w / 2, mouse_y = screen_h / 2;

    enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } drag_mode = DRAG_NONE;
    pude_window_t *drag_win = NULL;
    int drag_off_x = 0, drag_off_y = 0;
    int resize_base_w = 0, resize_base_h = 0, resize_base_mx = 0, resize_base_my = 0;

    pude_window_t *mouse_down_target = NULL;
    int mouse_down_cx = 0, mouse_down_cy = 0; /* target window's client origin at press time */

    render_frame(win, surface, menu_open, mouse_x, mouse_y);

    while (!quit) {
        bool had_event = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            had_event = true;
            switch (ev.type) {
            case SDL_QUIT:
                quit = true;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                SDL_Scancode sc = ev.key.keysym.scancode;
                bool down = (ev.type == SDL_KEYDOWN);
                if (is_modifier_scancode(sc)) {
                    if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) {
                        mods.shift = down;
                    } else {
                        mods.ctrl = down;
                    }
                } else if (down) {
                    if (mods.ctrl && sc == SDL_SCANCODE_F12) {
                        /* Emergency whole-desktop quit -- see docs/pude.md. */
                        quit = true;
                    } else if (!menu_open && win_count > 0) {
                        pude_window_t *top = win_order[win_count - 1];
                        if (top->cls->on_key) {
                            top->cls->on_key(top, top->state, sc, mods, true);
                        }
                    }
                }
                break;
            }

            case SDL_MOUSEMOTION:
                mouse_x = ev.motion.x;
                mouse_y = ev.motion.y;
                if (drag_mode == DRAG_NONE && mouse_down_target &&
                    mouse_down_target->cls->on_mouse_move) {
                    mouse_down_target->cls->on_mouse_move(mouse_down_target, mouse_down_target->state,
                                                           mouse_x - mouse_down_cx,
                                                           mouse_y - mouse_down_cy);
                }
                if (drag_mode == DRAG_MOVE && drag_win) {
                    drag_win->x = mouse_x - drag_off_x;
                    drag_win->y = mouse_y - drag_off_y;
                    if (drag_win->x < 0) drag_win->x = 0;
                    if (drag_win->y < 0) drag_win->y = 0;
                    if (drag_win->x > screen_w - drag_win->w) drag_win->x = screen_w - drag_win->w;
                    if (drag_win->y > screen_h - drag_win->h) drag_win->y = screen_h - drag_win->h;
                } else if (drag_mode == DRAG_RESIZE && drag_win) {
                    int min_w = drag_win->cls->min_client_w + 2 * BORDER;
                    int min_h = drag_win->cls->min_client_h + 2 * BORDER + titlebar_h();
                    int new_w = resize_base_w + (mouse_x - resize_base_mx);
                    int new_h = resize_base_h + (mouse_y - resize_base_my);
                    if (new_w < min_w) new_w = min_w;
                    if (new_h < min_h) new_h = min_h;
                    if (drag_win->x + new_w > screen_w) new_w = screen_w - drag_win->x;
                    if (drag_win->y + new_h > screen_h) new_h = screen_h - drag_win->y;
                    drag_win->w = new_w;
                    drag_win->h = new_h;
                }
                break;

            case SDL_MOUSEBUTTONDOWN: {
                if (ev.button.button != SDL_BUTTON_LEFT) {
                    break;
                }
                int mx = ev.button.x, my = ev.button.y;

                if (menu_open) {
                    int py = MENU_BTN_Y + MENU_BTN_H + 4;
                    int ph = NUM_APPS * MENU_ITEM_H + 8;
                    if (point_in(mx, my, MENU_BTN_X, py, MENU_ITEM_W, ph)) {
                        int idx = (my - (py + 4)) / MENU_ITEM_H;
                        if (idx >= 0 && idx < NUM_APPS) {
                            spawn_window(g_apps[idx], screen_w, screen_h);
                        }
                    }
                    menu_open = false;
                    break;
                }

                if (point_in(mx, my, MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H)) {
                    menu_open = true;
                    break;
                }

                for (int i = win_count - 1; i >= 0; i--) {
                    pude_window_t *w = win_order[i];
                    if (!point_in(mx, my, w->x, w->y, w->w, w->h)) {
                        continue;
                    }
                    bring_to_front(i);

                    int bx, by, bw, bh;
                    close_button_rect(w, &bx, &by, &bw, &bh);
                    int gx0 = w->x + w->w - RESIZE_GRIP, gy0 = w->y + w->h - RESIZE_GRIP;
                    int cx, cy, cw, ch;
                    window_client_rect(w, &cx, &cy, &cw, &ch);

                    if (point_in(mx, my, bx, by, bw, bh)) {
                        int z = win_count - 1; /* just brought to front */
                        /* confirm_close == NULL keeps every existing app's
                         * old behavior (close immediately); an app that
                         * has one and returns false is expected to have
                         * put up its own in-window modal as a side effect
                         * of this same call -- see pude_app.h. */
                        if (!w->cls->confirm_close || w->cls->confirm_close(w, w->state)) {
                            close_window_at(z);
                        }
                    } else if (point_in(mx, my, gx0, gy0, RESIZE_GRIP, RESIZE_GRIP)) {
                        drag_mode = DRAG_RESIZE;
                        drag_win = w;
                        resize_base_w = w->w;
                        resize_base_h = w->h;
                        resize_base_mx = mx;
                        resize_base_my = my;
                    } else if (my < w->y + BORDER + titlebar_h() && my >= w->y + BORDER) {
                        drag_mode = DRAG_MOVE;
                        drag_win = w;
                        drag_off_x = mx - w->x;
                        drag_off_y = my - w->y;
                    } else if (point_in(mx, my, cx, cy, cw, ch)) {
                        mouse_down_target = w;
                        mouse_down_cx = cx;
                        mouse_down_cy = cy;
                        if (w->cls->on_mouse_down) {
                            w->cls->on_mouse_down(w, w->state, mx - cx, my - cy);
                        }
                    }
                    break;
                }
                break;
            }

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button != SDL_BUTTON_LEFT) {
                    break;
                }
                if (drag_mode == DRAG_RESIZE && drag_win) {
                    int cx, cy, cw, ch;
                    window_client_rect(drag_win, &cx, &cy, &cw, &ch);
                    if (drag_win->cls->on_resize) {
                        drag_win->cls->on_resize(drag_win, drag_win->state, cw, ch);
                    }
                } else if (mouse_down_target && mouse_down_target->cls->on_mouse_up) {
                    mouse_down_target->cls->on_mouse_up(mouse_down_target, mouse_down_target->state,
                                                         ev.button.x - mouse_down_cx,
                                                         ev.button.y - mouse_down_cy);
                }
                drag_mode = DRAG_NONE;
                drag_win = NULL;
                mouse_down_target = NULL;
                break;

            default:
                break;
            }
        }

        /* Drains pude_spawn.h's one-slot mailbox -- e.g. PUFiles asking
         * for a new PUTerm window preloaded with an editor command when
         * the user opens a plain-text file (docs/pude.md's "Opening
         * files" section). Only user/pude.c's spawn_window() may
         * allocate a window-pool slot, so this is the one place any such
         * request actually gets acted on. */
        const app_class_t *spawn_cls = NULL;
        char spawn_cmd[PUDE_SPAWN_CMD_MAX];
        if (pude_take_spawn_request(&spawn_cls, spawn_cmd, sizeof(spawn_cmd))) {
            if (spawn_cls == &puterm_app_class) {
                puterm_set_startup_command(spawn_cmd);
            } else if (spawn_cls == &putext_app_class) {
                putext_set_startup_path(spawn_cmd);
            }
            spawn_window(spawn_cls, screen_w, screen_h);
            had_event = true;
        }

        bool need_redraw = had_event || (drag_mode != DRAG_NONE);

        for (int i = 0; i < win_count; i++) {
            pude_window_t *w = win_order[i];
            if (w->cls->poll && w->cls->poll(w, w->state)) {
                need_redraw = true;
            }
        }
        /* Auto-close any window whose app ended on its own (e.g. PUTerm's
         * shell exited via `exit`) -- exactly like a user-initiated close,
         * and never takes the rest of the desktop down with it. */
        for (int i = win_count - 1; i >= 0; i--) {
            pude_window_t *w = win_order[i];
            if ((w->cls->is_alive && !w->cls->is_alive(w, w->state)) || w->self_close_request) {
                close_window_at(i);
                need_redraw = true;
            }
        }

        if (need_redraw) {
            render_frame(win, surface, menu_open, mouse_x, mouse_y);
        }

        SDL_Delay(16);
    }

    for (int i = win_count - 1; i >= 0; i--) {
        close_window_at(i);
    }

    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
