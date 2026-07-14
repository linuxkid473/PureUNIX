#ifndef PUDE_WIDGETS_H
#define PUDE_WIDGETS_H

/* Small, general-purpose ring-3 widget primitives shared by `pude` apps
 * that need more than pude_gfx.h's raw pixel drawing -- buttons, a plain
 * single-line text input, and a scancode->ASCII mapping for typing into
 * one. Extracted while building PUFiles (docs/pude.md), which needed all
 * three (toolbar buttons, New Folder/Rename name entry) and found that
 * user/pude_term.c already had its own private scancode->character table
 * for the same purpose (encoding printable keys before turning them into
 * pty bytes) -- reusing one table here means PUTerm and PUFiles never
 * drift out of sync on which physical key produces which character.
 *
 * Header-only (`static inline`), same convention as pude_gfx.h, so a new
 * app never needs a new Makefile object rule just to use these.
 *
 * Deliberately NOT a full GUI toolkit: no layout engine, no widget tree,
 * no event dispatch of its own -- just the handful of pieces that were
 * actually about to be duplicated. Add more only when a second real app
 * needs the same thing a third time.
 */
#include "pude_app.h"
#include "pude_gfx.h"
#include <SDL.h>
#include <stdbool.h>
#include <string.h>

/* ---- scancode -> plain ASCII (no escape sequences, unlike PUTerm's own
 * encode_key(), which also emits control bytes/CSI sequences for a real
 * terminal) -- just "what character would a text field insert". Returns
 * 0 for a scancode with no printable mapping (function keys, arrows, bare
 * modifiers, ...). ---- */
static inline char pu_scancode_to_ascii(SDL_Scancode sc, key_mods_t mods)
{
    static const struct { SDL_Scancode sc; char lo, hi; } table[] = {
        { SDL_SCANCODE_A, 'a', 'A' }, { SDL_SCANCODE_B, 'b', 'B' },
        { SDL_SCANCODE_C, 'c', 'C' }, { SDL_SCANCODE_D, 'd', 'D' },
        { SDL_SCANCODE_E, 'e', 'E' }, { SDL_SCANCODE_F, 'f', 'F' },
        { SDL_SCANCODE_G, 'g', 'G' }, { SDL_SCANCODE_H, 'h', 'H' },
        { SDL_SCANCODE_I, 'i', 'I' }, { SDL_SCANCODE_J, 'j', 'J' },
        { SDL_SCANCODE_K, 'k', 'K' }, { SDL_SCANCODE_L, 'l', 'L' },
        { SDL_SCANCODE_M, 'm', 'M' }, { SDL_SCANCODE_N, 'n', 'N' },
        { SDL_SCANCODE_O, 'o', 'O' }, { SDL_SCANCODE_P, 'p', 'P' },
        { SDL_SCANCODE_Q, 'q', 'Q' }, { SDL_SCANCODE_R, 'r', 'R' },
        { SDL_SCANCODE_S, 's', 'S' }, { SDL_SCANCODE_T, 't', 'T' },
        { SDL_SCANCODE_U, 'u', 'U' }, { SDL_SCANCODE_V, 'v', 'V' },
        { SDL_SCANCODE_W, 'w', 'W' }, { SDL_SCANCODE_X, 'x', 'X' },
        { SDL_SCANCODE_Y, 'y', 'Y' }, { SDL_SCANCODE_Z, 'z', 'Z' },
        { SDL_SCANCODE_1, '1', '!' }, { SDL_SCANCODE_2, '2', '@' },
        { SDL_SCANCODE_3, '3', '#' }, { SDL_SCANCODE_4, '4', '$' },
        { SDL_SCANCODE_5, '5', '%' }, { SDL_SCANCODE_6, '6', '^' },
        { SDL_SCANCODE_7, '7', '&' }, { SDL_SCANCODE_8, '8', '*' },
        { SDL_SCANCODE_9, '9', '(' }, { SDL_SCANCODE_0, '0', ')' },
        { SDL_SCANCODE_MINUS, '-', '_' }, { SDL_SCANCODE_EQUALS, '=', '+' },
        { SDL_SCANCODE_LEFTBRACKET, '[', '{' }, { SDL_SCANCODE_RIGHTBRACKET, ']', '}' },
        { SDL_SCANCODE_BACKSLASH, '\\', '|' }, { SDL_SCANCODE_SEMICOLON, ';', ':' },
        { SDL_SCANCODE_APOSTROPHE, '\'', '"' }, { SDL_SCANCODE_GRAVE, '`', '~' },
        { SDL_SCANCODE_COMMA, ',', '<' }, { SDL_SCANCODE_PERIOD, '.', '>' },
        { SDL_SCANCODE_SLASH, '/', '?' }, { SDL_SCANCODE_SPACE, ' ', ' ' },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].sc == sc) {
            return mods.shift ? table[i].hi : table[i].lo;
        }
    }
    return 0;
}

/* ---- Button: a labeled rectangle -- hit-test + draw, no state of its
 * own (the caller owns whatever pressing it should do). ---- */
typedef struct {
    int x, y, w, h;
    const char *label;
} pu_button_t;

static inline bool pu_button_hit(const pu_button_t *b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static inline void pu_button_draw(SDL_Surface *s, const pu_button_t *b, bool enabled,
                                    Uint32 face, Uint32 text_col)
{
    Uint32 actual_face = enabled ? face : SDL_MapRGB(s->format, 45, 47, 52);
    Uint32 actual_text = enabled ? text_col : SDL_MapRGB(s->format, 110, 112, 118);
    pu_fill_rect(s, b->x, b->y, b->w, b->h, actual_face);
    pu_draw_rect_outline(s, b->x, b->y, b->w, b->h, 1, SDL_MapRGB(s->format, 15, 17, 22));

    /* Centered, but always via pu_draw_string_clipped (bounded to the
     * button's own width) rather than pu_draw_string_centered (which
     * doesn't clip at all) -- a label longer than a narrow button
     * (possible after a drag-resize down to an app's min_client_w) must
     * never bleed into a neighboring button or off the window's edge. */
    int len = (int)strlen(b->label);
    int avail = b->w > 4 ? b->w - 4 : 0;
    int max_chars = avail / FONT_CELL_W;
    int shown = len < max_chars ? len : max_chars;
    int tw = shown * FONT_CELL_W;
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - FONT_CELL_H) / 2;
    if (tx < b->x) tx = b->x;
    if (ty < b->y) ty = b->y;
    pu_draw_string_clipped(s, tx, ty, b->x + b->w - tx, b->label, actual_text, actual_face);
}

/* ---- Single-line text input: append/backspace at the end only (no
 * mid-string cursor placement -- every current caller only ever needs to
 * type or correct a short name, and this keeps the widget's state and
 * key handling trivial). ---- */
#define PU_TEXTINPUT_MAX 128

typedef struct {
    char buf[PU_TEXTINPUT_MAX];
    int len;
} pu_textinput_t;

static inline void pu_textinput_set(pu_textinput_t *ti, const char *initial)
{
    strncpy(ti->buf, initial ? initial : "", PU_TEXTINPUT_MAX - 1);
    ti->buf[PU_TEXTINPUT_MAX - 1] = '\0';
    ti->len = (int)strlen(ti->buf);
}

static inline void pu_textinput_backspace(pu_textinput_t *ti)
{
    if (ti->len > 0) {
        ti->buf[--ti->len] = '\0';
    }
}

/* Returns true if the character was accepted (false if the field is
 * already full or the char isn't insertable, e.g. NUL). */
static inline bool pu_textinput_putc(pu_textinput_t *ti, char c)
{
    if (c == 0 || ti->len >= PU_TEXTINPUT_MAX - 1) {
        return false;
    }
    ti->buf[ti->len++] = c;
    ti->buf[ti->len] = '\0';
    return true;
}

/* Draws the field's box plus its current contents and a trailing block
 * cursor -- clipped via pu_draw_string_clipped so a name typed longer
 * than the box is wide never overflows it. */
static inline void pu_textinput_draw(SDL_Surface *s, int x, int y, int w, int h,
                                       const pu_textinput_t *ti)
{
    pu_fill_rect(s, x, y, w, h, SDL_MapRGB(s->format, 15, 17, 22));
    pu_draw_rect_outline(s, x, y, w, h, 1, SDL_MapRGB(s->format, 150, 155, 165));
    int tx = x + 4;
    int ty = y + (h - FONT_CELL_H) / 2;
    pu_draw_string_clipped(s, tx, ty, w - 8 - FONT_CELL_W, ti->buf, 0xE0FFE0, 0x0F1116);
    int cursor_x = tx + ti->len * FONT_CELL_W;
    if (cursor_x < x + w - FONT_CELL_W) {
        pu_fill_rect(s, cursor_x, ty, 2, FONT_CELL_H, 0xC0FFC0);
    }
}

/* ---- Tiny scrollable-list arithmetic -- how many rows fit, and which
 * row index a click at a given y lands on. Kept as plain functions
 * (rather than owning the item data itself) since every real caller's
 * row content differs completely. ---- */
static inline int pu_list_visible_rows(int area_h, int row_h)
{
    return row_h > 0 ? area_h / row_h : 0;
}

/* Returns the item index for a click at (rel_y) within the list area
 * (rel_y == 0 at the list's own top), or -1 if it's out of range. */
static inline int pu_list_row_at(int rel_y, int row_h, int scroll_offset, int item_count)
{
    if (row_h <= 0 || rel_y < 0) {
        return -1;
    }
    int idx = scroll_offset + rel_y / row_h;
    if (idx < 0 || idx >= item_count) {
        return -1;
    }
    return idx;
}

#endif
