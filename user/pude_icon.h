#ifndef PUDE_ICON_H
#define PUDE_ICON_H

/* Reusable procedural icon-drawing abstraction for `pude`'s desktop shell
 * (the dock and app drawer added on top of the existing window manager,
 * docs/pude.md). An "icon" is just a function pointer stored on an app's
 * own `app_class_t` (user/pude_app.h's `icon_draw` field) -- the dock and
 * app drawer call it through `pude_icon_draw_fn` and never know or care
 * what shape a given app's icon actually is. This mirrors pude_gfx.h's
 * convention (header-only, `static inline`, one copy per translation
 * unit) so registering a new app's icon never needs a new Makefile object
 * rule.
 *
 * Every icon is drawn procedurally with pude_gfx.h's own primitives
 * (rects, outlines, raw pixels) scaled to whatever (w,h) box the caller
 * hands it -- no bitmap assets, no font glyphs (text pretending to be an
 * icon reads wrong at small sizes and doesn't restyle with the desktop),
 * no dependency on any resolution-specific layout. A shared tile background
 * (pu_icon_tile below) gives every icon the same hover/pressed feedback and
 * border style so the whole set reads as one coherent icon language rather
 * than four unrelated drawings.
 */
#include "pude_gfx.h"
#include <SDL.h>
#include <stdbool.h>

typedef void (*pude_icon_draw_fn)(SDL_Surface *s, int x, int y, int w, int h,
                                   bool hovered, bool pressed);

/* Shared background tile every icon in this file draws itself onto --
 * gives the dock/drawer one consistent "button" language (flat face,
 * lighten on hover, darken + inset border when pressed) instead of each
 * icon inventing its own feedback style. Returns the largest centered
 * square sub-rect (via out-params sz, ox, oy) so callers can lay out icon
 * content
 * proportionally regardless of the (w,h) box's exact aspect ratio. */
static inline void pu_icon_tile(SDL_Surface *s, int x, int y, int w, int h,
                                 bool hovered, bool pressed,
                                 int *sz, int *ox, int *oy)
{
    Uint32 face = pressed ? SDL_MapRGB(s->format, 35, 38, 45)
                : hovered  ? SDL_MapRGB(s->format, 78, 85, 100)
                           : SDL_MapRGB(s->format, 55, 60, 70);
    Uint32 border = hovered ? SDL_MapRGB(s->format, 190, 195, 205)
                             : SDL_MapRGB(s->format, 120, 124, 132);
    pu_fill_rect(s, x, y, w, h, face);
    pu_draw_rect_outline(s, x, y, w, h, 1, border);
    if (pressed) {
        pu_draw_rect_outline(s, x + 2, y + 2, w - 4, h - 4, 1,
                              SDL_MapRGB(s->format, 15, 17, 22));
    }

    int s0 = w < h ? w : h;
    *sz = s0;
    *ox = x + (w - s0) / 2;
    *oy = y + (h - s0) / 2;
}

/* ---- PUText: a page with a folded corner and a few text-line strokes. --- */
static inline void pu_icon_putext(SDL_Surface *s, int x, int y, int w, int h,
                                   bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    int pw = sz * 5 / 10, ph = sz * 7 / 10;
    int px = ox + (sz - pw) / 2, py = oy + (sz - ph) / 2;
    Uint32 page = SDL_MapRGB(s->format, 235, 232, 220);
    Uint32 edge = SDL_MapRGB(s->format, 120, 116, 100);
    pu_fill_rect(s, px, py, pw, ph, page);
    pu_draw_rect_outline(s, px, py, pw, ph, 1, edge);

    /* Folded top-right corner: same shrinking-triangle stipple user/pude.c
     * already uses for its window resize grip, reused here so a second,
     * unrelated triangle-drawing technique doesn't need inventing. */
    int fold = sz * 2 / 10;
    if (fold >= 3) {
        Uint32 fold_col = SDL_MapRGB(s->format, 200, 196, 180);
        for (int i = 0; i < fold; i++) {
            pu_fill_rect(s, px + pw - fold + i, py + i, fold - i, 1, fold_col);
        }
    }

    int lm = pw / 6;
    int ly = py + ph * 4 / 10;
    int lh = sz / 22 > 0 ? sz / 22 : 1;
    Uint32 line_col = SDL_MapRGB(s->format, 90, 88, 78);
    for (int i = 0; i < 3; i++) {
        int lw = pw - 2 * lm - (i == 2 ? pw / 4 : 0);
        if (lw > 0) {
            pu_fill_rect(s, px + lm, ly + i * (lh + lh + 1), lw, lh, line_col);
        }
    }
}

/* ---- Calculator: body, a small display, and a button grid. ---- */
static inline void pu_icon_calc(SDL_Surface *s, int x, int y, int w, int h,
                                 bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    int bw = sz * 6 / 10, bh = sz * 8 / 10;
    int bx = ox + (sz - bw) / 2, by = oy + (sz - bh) / 2;
    pu_fill_rect(s, bx, by, bw, bh, SDL_MapRGB(s->format, 35, 37, 42));
    pu_draw_rect_outline(s, bx, by, bw, bh, 1, SDL_MapRGB(s->format, 15, 17, 22));

    int dm = bw / 8 > 1 ? bw / 8 : 1;
    int dh = bh * 2 / 10 > 3 ? bh * 2 / 10 : 3;
    pu_fill_rect(s, bx + dm, by + dm, bw - 2 * dm, dh, SDL_MapRGB(s->format, 150, 220, 160));

    int grid_y = by + dm + dh + dm;
    int grid_h = by + bh - dm - grid_y;
    int rows = 3, cols = 3;
    if (grid_h > 0) {
        int cell_w = (bw - 2 * dm) / cols;
        int cell_h = grid_h / rows;
        Uint32 key_col = SDL_MapRGB(s->format, 90, 94, 104);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int kx = bx + dm + c * cell_w;
                int ky = grid_y + r * cell_h;
                int kw = cell_w - (cell_w > 3 ? 2 : 0);
                int kh = cell_h - (cell_h > 3 ? 2 : 0);
                if (kw > 0 && kh > 0) {
                    pu_fill_rect(s, kx, ky, kw, kh, key_col);
                }
            }
        }
    }
}

/* ---- PUFiles: a two-piece folder (tab + body). ---- */
static inline void pu_icon_pufiles(SDL_Surface *s, int x, int y, int w, int h,
                                    bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    Uint32 folder = SDL_MapRGB(s->format, 224, 176, 76);
    Uint32 edge = SDL_MapRGB(s->format, 140, 104, 32);

    int bw = sz * 7 / 10, bh = sz * 5 / 10;
    int bx = ox + (sz - bw) / 2, by = oy + sz - bh - (sz - bh - bh * 2 / 5 > 0 ? sz / 10 : 0);
    if (by + bh > oy + sz) by = oy + sz - bh;

    int tabw = bw * 4 / 10, tabh = sz * 12 / 100 > 2 ? sz * 12 / 100 : 2;
    int tabx = bx, taby = by - tabh;
    pu_fill_rect(s, tabx, taby, tabw, tabh, folder);
    pu_draw_rect_outline(s, tabx, taby, tabw, tabh, 1, edge);

    pu_fill_rect(s, bx, by, bw, bh, folder);
    pu_draw_rect_outline(s, bx, by, bw, bh, 1, edge);
    /* A single horizontal crease near the top reads as a folder's front
     * flap without needing a second overlapping polygon. */
    pu_fill_rect(s, bx + 1, by + bh / 5, bw - 2, 1, edge);
}

/* ---- PUTerm: a dark screen with a prompt chevron and a cursor block. --- */
static inline void pu_icon_puterm(SDL_Surface *s, int x, int y, int w, int h,
                                   bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    int bw = sz * 8 / 10, bh = sz * 7 / 10;
    int bx = ox + (sz - bw) / 2, by = oy + (sz - bh) / 2;
    pu_fill_rect(s, bx, by, bw, bh, SDL_MapRGB(s->format, 12, 14, 18));
    pu_draw_rect_outline(s, bx, by, bw, bh, 1, SDL_MapRGB(s->format, 150, 155, 165));

    Uint32 prompt = SDL_MapRGB(s->format, 90, 220, 120);
    int cw = bh / 6 > 1 ? bh / 6 : 1;   /* chevron stroke thickness */
    int clen = bh * 3 / 10 > 2 ? bh * 3 / 10 : 2;
    int gx = bx + bw / 8, gy = by + bh / 2 - clen;
    for (int i = 0; i < clen; i++) {
        pu_fill_rect(s, gx + i, gy + i, cw, cw, prompt);
        pu_fill_rect(s, gx + i, gy + 2 * clen - i, cw, cw, prompt);
    }

    int curw = bw / 4, curh = cw;
    pu_fill_rect(s, gx + clen + cw, by + bh / 2 - curh / 2, curw, curh, prompt);
}

/* ---- Settings: a gear -- a square hub ringed by eight stubby teeth, plus
 * a recessed center hole. Every other icon in this file is built the same
 * way (nested pu_fill_rect() calls, no circle primitive exists in
 * pude_gfx.h), so a gear reads as "square teeth around a square body"
 * rather than a true polygon -- legible at dock/drawer sizes without
 * needing a new drawing primitive just for this one icon. ---- */
static inline void pu_icon_settings(SDL_Surface *s, int x, int y, int w, int h,
                                     bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    Uint32 body = SDL_MapRGB(s->format, 175, 180, 190);
    Uint32 edge = SDL_MapRGB(s->format, 90, 94, 104);
    Uint32 hole = SDL_MapRGB(s->format, 40, 42, 50);

    int hub = sz * 5 / 10;
    int hx = ox + (sz - hub) / 2, hy = oy + (sz - hub) / 2;

    int tooth = sz * 16 / 100 > 2 ? sz * 16 / 100 : 2;
    /* Eight teeth: the four edge-midpoints plus the four corners, each a
     * small square straddling the hub's own border. */
    struct { int cx, cy; } teeth[8] = {
        { hx + hub / 2, hy },                 /* top */
        { hx + hub / 2, hy + hub },            /* bottom */
        { hx, hy + hub / 2 },                  /* left */
        { hx + hub, hy + hub / 2 },             /* right */
        { hx, hy },                             /* top-left */
        { hx + hub, hy },                       /* top-right */
        { hx, hy + hub },                       /* bottom-left */
        { hx + hub, hy + hub },                 /* bottom-right */
    };
    for (int i = 0; i < 8; i++) {
        pu_fill_rect(s, teeth[i].cx - tooth / 2, teeth[i].cy - tooth / 2, tooth, tooth, body);
    }

    pu_fill_rect(s, hx, hy, hub, hub, body);
    pu_draw_rect_outline(s, hx, hy, hub, hub, 1, edge);

    int hole_sz = hub * 4 / 10 > 2 ? hub * 4 / 10 : 2;
    pu_fill_rect(s, hx + (hub - hole_sz) / 2, hy + (hub - hole_sz) / 2, hole_sz, hole_sz, hole);
}

/* ---- App-drawer button: a 3x3 grid of dots, distinguishing it from any
 * single app's icon at a glance. ---- */
static inline void pu_icon_drawer(SDL_Surface *s, int x, int y, int w, int h,
                                   bool hovered, bool pressed)
{
    int sz, ox, oy;
    pu_icon_tile(s, x, y, w, h, hovered, pressed, &sz, &ox, &oy);

    int cell = sz / 4;
    int dot = cell / 2 > 1 ? cell / 2 : 1;
    int gx0 = ox + (sz - 3 * cell) / 2;
    int gy0 = oy + (sz - 3 * cell) / 2;
    Uint32 dot_col = SDL_MapRGB(s->format, 220, 224, 232);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            pu_fill_rect(s, gx0 + c * cell, gy0 + r * cell, dot, dot, dot_col);
        }
    }
}

#endif
