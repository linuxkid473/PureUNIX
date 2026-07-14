/* user/pude_calc.c — a real ring-3 GUI calculator app for `pude`.
 *
 * Pure userspace arithmetic and rendering, no kernel involvement at all
 * (see pude_calc.h) -- plugs into the WM through the same app_class_t
 * (user/pude_app.h) PUTerm does. Layout is computed from the window's
 * current client size every time it's needed (create/render/on_resize/
 * on_mouse_down all recompute from state->cw/ch), so a resize re-lays out
 * the button grid rather than stretching a fixed-resolution rendering.
 */
#include "pude_calc.h"
#include "pude_gfx.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_H   48
#define GRID_ROWS   5
#define GRID_COLS   4
#define DISPLAY_MAX 24

/* Row 4 (the last one) is a single "C" (clear) button spanning the full
 * width -- represented here with the same label repeated in every column
 * so hit-testing (which always looks up btn_labels[row][col]) doesn't
 * need a special case, only render() draws it as one wide button. */
static const char *btn_labels[GRID_ROWS][GRID_COLS] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "0", ".", "=", "+" },
    { "C", "C", "C", "C" },
};

typedef struct {
    char display[DISPLAY_MAX];
    double accumulator;
    char pending_op; /* 0 = none */
    bool entering;   /* mid-typing a new operand */
    bool error;
    int cw, ch;      /* last-known client size, for hit-testing */
} calc_state_t;

static void calc_reset(calc_state_t *st)
{
    strcpy(st->display, "0");
    st->accumulator = 0.0;
    st->pending_op = 0;
    st->entering = false;
    st->error = false;
}

static double calc_parse_display(calc_state_t *st)
{
    return strtod(st->display, NULL);
}

static void calc_format(calc_state_t *st, double v)
{
    snprintf(st->display, sizeof(st->display), "%.10g", v);
}

static double apply_op(double a, double b, char op, bool *out_error)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0.0) {
            *out_error = true;
            return 0.0;
        }
        return a / b;
    default:
        return b;
    }
}

static void calc_digit(calc_state_t *st, char d)
{
    if (!st->entering) {
        strcpy(st->display, "0");
        st->entering = true;
    }
    size_t len = strlen(st->display);
    bool is_zero = (len == 1 && st->display[0] == '0');
    if (is_zero && d != '.') {
        st->display[0] = d;
        return;
    }
    if (len + 1 >= sizeof(st->display)) {
        return; /* full -- ignore further digits, like a real calculator */
    }
    st->display[len] = d;
    st->display[len + 1] = '\0';
}

static void calc_decimal(calc_state_t *st)
{
    if (!st->entering) {
        strcpy(st->display, "0");
        st->entering = true;
    }
    if (strchr(st->display, '.')) {
        return; /* one decimal point only */
    }
    calc_digit(st, '.');
}

static void calc_operator(calc_state_t *st, char op)
{
    double val = calc_parse_display(st);
    if (st->pending_op && st->entering) {
        bool err = false;
        st->accumulator = apply_op(st->accumulator, val, st->pending_op, &err);
        if (err) {
            st->error = true;
            strcpy(st->display, "Error");
            st->pending_op = 0;
            st->entering = false;
            return;
        }
    } else {
        st->accumulator = val;
    }
    st->pending_op = op;
    st->entering = false;
    calc_format(st, st->accumulator);
}

static void calc_equals(calc_state_t *st)
{
    if (!st->pending_op) {
        return;
    }
    double val = calc_parse_display(st);
    bool err = false;
    double result = apply_op(st->accumulator, val, st->pending_op, &err);
    if (err) {
        st->error = true;
        strcpy(st->display, "Error");
        st->pending_op = 0;
        st->entering = false;
        return;
    }
    st->accumulator = result;
    st->pending_op = 0;
    st->entering = false;
    calc_format(st, result);
}

static void calc_press(calc_state_t *st, const char *label)
{
    if (st->error && strcmp(label, "C") != 0) {
        calc_reset(st);
    }
    if (strcmp(label, "C") == 0) {
        calc_reset(st);
    } else if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') {
        calc_digit(st, label[0]);
    } else if (strcmp(label, ".") == 0) {
        calc_decimal(st);
    } else if (strcmp(label, "=") == 0) {
        calc_equals(st);
    } else if (strcmp(label, "+") == 0 || strcmp(label, "-") == 0 ||
               strcmp(label, "*") == 0 || strcmp(label, "/") == 0) {
        calc_operator(st, label[0]);
    }
}

/* ---- app_class_t callbacks ------------------------------------------------ */

static void *calc_create(pude_window_t *win, int client_w, int client_h)
{
    (void)win;
    calc_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    calc_reset(st);
    st->cw = client_w;
    st->ch = client_h;
    return st;
}

static void calc_destroy(pude_window_t *win, void *state)
{
    (void)win;
    free(state);
}

static void calc_render(pude_window_t *win, void *state, SDL_Surface *s,
                         int cx, int cy, int cw, int ch)
{
    (void)win;
    calc_state_t *st = state;

    pu_fill_rect(s, cx, cy, cw, ch, SDL_MapRGB(s->format, 30, 34, 44));

    int display_h = DISPLAY_H;
    if (display_h > ch) display_h = ch;
    pu_fill_rect(s, cx, cy, cw, display_h, SDL_MapRGB(s->format, 10, 12, 16));
    pu_draw_rect_outline(s, cx, cy, cw, display_h, 2, SDL_MapRGB(s->format, 80, 84, 94));
    int text_w = (int)strlen(st->display) * FONT_CELL_W;
    int text_x = cx + cw - text_w - 10;
    if (text_x < cx + 4) text_x = cx + 4;
    pu_draw_string(s, text_x, cy + (display_h - FONT_CELL_H) / 2, st->display,
                    st->error ? 0xFF6060 : 0xE0FFE0, 0x0A0C10);

    int grid_h = ch - display_h;
    if (grid_h < 0) grid_h = 0;
    int row_h = grid_h / GRID_ROWS;
    int col_w = cw / GRID_COLS;

    for (int r = 0; r < GRID_ROWS; r++) {
        int by = cy + display_h + r * row_h;
        bool wide = (r == GRID_ROWS - 1);
        int cols_here = wide ? 1 : GRID_COLS;
        int bw = wide ? cw : col_w;
        for (int c = 0; c < cols_here; c++) {
            int bx = cx + c * bw;
            const char *label = btn_labels[r][c];
            bool is_op = (label[0] == '+' || label[0] == '-' ||
                          label[0] == '*' || label[0] == '/' || label[0] == '=');
            Uint32 face = wide   ? SDL_MapRGB(s->format, 130, 50, 50)
                          : is_op ? SDL_MapRGB(s->format, 60, 90, 140)
                                  : SDL_MapRGB(s->format, 55, 60, 72);
            pu_fill_rect(s, bx + 2, by + 2, bw - 4, row_h - 4, face);
            pu_draw_rect_outline(s, bx + 2, by + 2, bw - 4, row_h - 4, 1,
                                  SDL_MapRGB(s->format, 15, 17, 22));
            pu_draw_string_centered(s, bx, by, bw, row_h, label, 0xFFFFFF, face);
        }
    }
}

static void calc_on_mouse_down(pude_window_t *win, void *state, int x, int y)
{
    (void)win;
    calc_state_t *st = state;
    int display_h = DISPLAY_H;
    if (display_h > st->ch) display_h = st->ch;
    if (y < display_h) {
        return; /* clicking the display itself does nothing */
    }
    int grid_h = st->ch - display_h;
    if (grid_h <= 0) {
        return;
    }
    int row_h = grid_h / GRID_ROWS;
    int col_w = st->cw / GRID_COLS;
    if (row_h <= 0 || col_w <= 0) {
        return;
    }
    int row = (y - display_h) / row_h;
    int col = x / col_w;
    if (row < 0) row = 0;
    if (row >= GRID_ROWS) row = GRID_ROWS - 1;
    if (col < 0) col = 0;
    if (col >= GRID_COLS) col = GRID_COLS - 1;
    calc_press(st, btn_labels[row][col]);
}

static void calc_on_resize(pude_window_t *win, void *state, int new_client_w, int new_client_h)
{
    (void)win;
    calc_state_t *st = state;
    st->cw = new_client_w;
    st->ch = new_client_h;
}

const app_class_t calc_app_class = {
    .name = "Calculator",
    .default_client_w = 240,
    .default_client_h = 320,
    .min_client_w = 160,
    .min_client_h = 220,
    .create = calc_create,
    .destroy = calc_destroy,
    .render = calc_render,
    .on_key = NULL,
    .on_mouse_down = calc_on_mouse_down,
    .on_mouse_up = NULL,
    .on_resize = calc_on_resize,
    .poll = NULL,
    .is_alive = NULL,
};
