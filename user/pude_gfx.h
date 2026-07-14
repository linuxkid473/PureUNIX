#ifndef PUDE_GFX_H
#define PUDE_GFX_H

/* Shared pixel-level drawing primitives for every `pude` desktop
 * component (the WM's own chrome in user/pude.c, PUTerm in
 * user/pude_term.c, Calculator in user/pude_calc.c) -- header-only
 * (`static inline`, one copy compiled per translation unit) so adding a
 * new app never needs a new Makefile object rule just to get text/rect
 * drawing.
 *
 * Reuses drivers/font.c's exact pre-rasterized Menlo glyph bitmap (the
 * same one the VGA text console uses) via `font_glyph()` -- see
 * user/pude.c's original comment (preserved below) for why that source
 * file is compiled as its own object and linked in rather than
 * #include'd normally.
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* drivers/font.c is compiled as its own translation unit (see Makefile)
 * and linked in directly -- its own header, include/pureunix/font.h,
 * can't be #include'd here: it transitively pulls in
 * include/pureunix/types.h, whose kernel-style uid_t/gid_t/ino_t/...
 * typedefs collide with newlib's own <sys/types.h> the moment both are
 * visible in one translation unit. Declaring just the narrow piece
 * needed here avoids that entirely. */
#define FONT_CELL_W 8
#define FONT_CELL_H 17
extern const uint8_t *font_glyph(char c);

static inline void pu_put_pixel(SDL_Surface *s, int x, int y, Uint32 pixel)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) {
        return;
    }
    Uint8 *row = (Uint8 *)s->pixels + (size_t)y * s->pitch;
    ((Uint32 *)row)[x] = pixel;
}

static inline void pu_fill_rect(SDL_Surface *s, int x, int y, int w, int h, Uint32 pixel)
{
    SDL_Rect r = { x, y, w, h };
    SDL_FillRect(s, &r, pixel);
}

/* An unfilled rectangle outline, `thick` pixels wide -- used for button/
 * panel borders where a flat SDL_FillRect would hide the interior. */
static inline void pu_draw_rect_outline(SDL_Surface *s, int x, int y, int w, int h,
                                         int thick, Uint32 pixel)
{
    pu_fill_rect(s, x, y, w, thick, pixel);
    pu_fill_rect(s, x, y + h - thick, w, thick, pixel);
    pu_fill_rect(s, x, y, thick, h, pixel);
    pu_fill_rect(s, x + w - thick, y, thick, h, pixel);
}

static inline void pu_draw_glyph(SDL_Surface *s, int x0, int y0, char ch, Uint32 fg, Uint32 bg)
{
    const uint8_t *g = font_glyph(ch);
    for (int row = 0; row < FONT_CELL_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT_CELL_W; col++) {
            bool on = (bits & (0x80 >> col)) != 0;
            pu_put_pixel(s, x0 + col, y0 + row, on ? fg : bg);
        }
    }
}

static inline void pu_draw_string(SDL_Surface *s, int x0, int y0, const char *str, Uint32 fg, Uint32 bg)
{
    int x = x0;
    for (const char *p = str; *p; p++) {
        pu_draw_glyph(s, x, y0, *p, fg, bg);
        x += FONT_CELL_W;
    }
}

/* Centers a short label inside a (w x h) box whose top-left is (x,y) --
 * used by button-style widgets (Calculator's keys, the launcher menu's
 * entries) so labels stay legible after a resize re-layout changes the
 * box size, rather than a fixed offset that could drift outside it. */
static inline void pu_draw_string_centered(SDL_Surface *s, int x, int y, int w, int h,
                                            const char *str, Uint32 fg, Uint32 bg)
{
    int len = 0;
    for (const char *p = str; *p; p++) len++;
    int tw = len * FONT_CELL_W;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_CELL_H) / 2;
    if (tx < x) tx = x;
    if (ty < y) ty = y;
    pu_draw_string(s, tx, ty, str, fg, bg);
}

#endif
