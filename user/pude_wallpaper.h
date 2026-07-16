#ifndef PUDE_WALLPAPER_H
#define PUDE_WALLPAPER_H

/* Desktop wallpaper for `pude` (docs/pude.md's "Settings" section) --
 * real upstream libpng/zlib decode (user/pude_wallpaper.c, same recipe as
 * user/imgview.c) of a user-chosen PNG file, scaled once (aspect-fill) to
 * the screen's real size and cached as a plain native-pixel-format buffer,
 * so the desktop compositor (user/pude.c's render_frame()) never re-
 * decodes or re-scales on any regular frame -- it just blits (or falls
 * back to the existing solid color) every time it redraws.
 *
 * Deliberately NOT header-only (unlike pude_gfx.h/pude_icon.h/pude_
 * widgets.h): the decoded cache and the currently-configured path are
 * real shared mutable state that must exist exactly once across the whole
 * `pude` binary, same convention as user/pude_spawn.c/user/pude_
 * clipboard.c.
 */
#include <SDL.h>
#include <stdbool.h>

#define PUDE_WALLPAPER_PATH_MAX 256

/* Call once at startup, right after the real screen surface exists (its
 * size and pixel format are cached for every later pude_wallpaper_set()
 * call, which is why this must run first). Reads /etc/pude.conf (docs/
 * pude.md's "Configuration file" section) and, if it names a wallpaper,
 * decodes and caches it immediately. A missing config file or an
 * unloadable wallpaper just leaves nothing cached (pude_wallpaper_render()
 * falls back to the existing solid desktop color) -- never a startup
 * failure. */
void pude_wallpaper_init(SDL_Surface *screen);

/* Decodes `path` (must be a real PNG file), scales it aspect-fill to the
 * screen size recorded by pude_wallpaper_init(), and caches the result --
 * replacing whatever wallpaper (if any) was cached before -- then
 * persists `path` to /etc/pude.conf so it survives reboot. On any failure
 * (bad path, not a real PNG, decode error, allocation failure) returns
 * false and leaves the previously-cached wallpaper untouched, so the
 * desktop keeps showing the last-known-good wallpaper (or the solid
 * fallback) instead of blanking on one bad file. */
bool pude_wallpaper_set(const char *path);

/* NULL if no wallpaper is currently applied (solid-color desktop). */
const char *pude_wallpaper_current_path(void);

/* Draws the cached wallpaper into `s`, or falls back to the same solid
 * desktop color user/pude.c's render_frame() used to fill unconditionally
 * -- called once per compositor redraw, never re-decoding or re-scaling
 * anything. */
void pude_wallpaper_render(SDL_Surface *s);

#endif
