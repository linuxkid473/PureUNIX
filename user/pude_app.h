#ifndef PUDE_APP_H
#define PUDE_APP_H

/* The generic application/window abstraction `pude` (docs/pude.md) uses so
 * PUTerm, the calculator, and any future ring-3 GUI program all plug into
 * the same window manager the same way -- no per-app special cases in
 * user/pude.c's own window list, event routing, or chrome rendering.
 *
 * An "app" is a `const app_class_t` describing its identity, default/
 * minimum *client* size (the drawable area inside a window's border/title
 * bar -- the WM adds chrome on top, so an app never needs to know border
 * thickness or title bar height), and a small set of lifecycle callbacks.
 * `pude_window_t` is the WM-owned per-instance record: geometry (whole
 * window, chrome included) plus the app's own opaque `state` pointer
 * returned by `create()`.
 *
 * Optional callbacks (an app that has no use for one just leaves it NULL --
 * user/pude.c always NULL-checks before calling): on_key, on_mouse_down,
 * on_mouse_up, poll, is_alive. create/destroy/render are mandatory.
 */
#include <SDL.h>
#include <stdbool.h>

typedef struct { bool shift, ctrl; } key_mods_t;

typedef struct pude_window pude_window_t;

typedef struct {
    const char *name; /* shown in the launcher menu and the title bar */

    /* Client-area (drawable, chrome-excluded) size in pixels. The WM adds
     * its own border/title-bar chrome around this to get the window's
     * whole on-screen size. */
    int default_client_w, default_client_h;
    int min_client_w, min_client_h;

    /* Returns an opaque app-owned state pointer, or NULL on failure (the WM
     * aborts the spawn if so). win->x/y/w/h are already set by the WM
     * before this is called; client_w/client_h are the exact drawable
     * dimensions (win's whole size minus chrome). */
    void *(*create)(pude_window_t *win, int client_w, int client_h);

    /* Releases everything the app owns -- for an app backed by a real
     * child process (PUTerm's shell), this is where it's actually
     * terminated and reaped, not just "forgotten". Called both when the
     * user clicks the close button and when the WM notices is_alive()
     * turned false on its own. */
    void (*destroy)(pude_window_t *win, void *state);

    /* Draws the app's client area into surface `s`, offset at (cx,cy),
     * sized (cw,ch) pixels -- chrome (border/title bar/resize grip) is
     * drawn separately by the WM. */
    void (*render)(pude_window_t *win, void *state, SDL_Surface *s,
                   int cx, int cy, int cw, int ch);

    /* Forwarded only for the focused (topmost) window, and only on
     * key-down (matches PUTerm/Calculator's needs; a future app needing
     * key-up would need this widened). */
    void (*on_key)(pude_window_t *win, void *state, SDL_Scancode sc,
                   key_mods_t mods, bool down);

    /* Mouse coordinates are client-relative (0,0 = client area's top-left)
     * -- an app never sees window or screen coordinates. */
    void (*on_mouse_down)(pude_window_t *win, void *state, int x, int y);
    void (*on_mouse_up)(pude_window_t *win, void *state, int x, int y);

    /* Forwarded on every mouse-motion event for as long as the left button
     * is held down *and* it was pressed inside this window's client area
     * in the first place (the WM tracks "which window owns this drag" from
     * its own on_mouse_down bookkeeping, same as on_mouse_up) -- never
     * called while the WM itself is using the drag for window move/resize.
     * Coordinates are client-relative like on_mouse_down/up but are NOT
     * clamped to the client rect (a drag can go past a window's own edge,
     * e.g. selecting text down to the last visible row); an app that cares
     * should clamp itself. This is the one general mechanism a click-and-
     * drag gesture (PUText's text selection) needs and PUFiles' click-only
     * interactions never did. */
    void (*on_mouse_move)(pude_window_t *win, void *state, int x, int y);

    /* Called once, after a mouse-driven resize completes, with the new
     * client-area size -- the one general-purpose mechanism every app uses
     * to learn its new drawable dimensions and re-layout/reflow. */
    void (*on_resize)(pude_window_t *win, void *state, int new_client_w, int new_client_h);

    /* Called once per WM frame regardless of input, so an app can poll a
     * backing resource (PUTerm's pty master). Returns true if it changed
     * state and the WM should redraw this frame. */
    bool (*poll)(pude_window_t *win, void *state);

    /* NULL means "never dies on its own" (Calculator) -- the WM only ever
     * closes it via the close button. Returning false (PUTerm: its shell
     * exited) makes the WM close the window automatically, exactly like a
     * user-initiated close. */
    bool (*is_alive)(pude_window_t *win, void *state);

    /* Optional gate on the close button (NULL = always OK to close right
     * away, the old behavior every existing app keeps). Called once when
     * the user clicks the close button; returning true lets the WM close
     * the window immediately exactly as before. Returning false tells the
     * WM to leave the window open -- the app is expected to have reacted
     * by switching into its own in-window confirmation modal (e.g.
     * PUText's "Discard unsaved changes?") as a side effect of this same
     * call. There is no second callback for "the user confirmed" -- an app
     * that wants to actually close after the user picks "Discard" sets
     * win->self_close_request (below) from whatever click/key handler
     * resolves that modal; the WM notices it on the very next frame. */
    bool (*confirm_close)(pude_window_t *win, void *state);
} app_class_t;

struct pude_window {
    const app_class_t *cls;
    void *state;
    char title[48];
    int x, y, w, h; /* whole window, chrome included; owned by the WM */

    /* WM-owned storage an app may set from any of its own callbacks (e.g.
     * a "Discard & Close" button inside its own confirm_close modal) to
     * ask the WM to close this exact window on its next frame -- the
     * general escape hatch for "I already got the user's confirmation
     * myself, now actually close me", since only user/pude.c's own
     * close_window_at() may ever remove a window from the pool. Bypasses
     * confirm_close entirely (it already did its job). */
    bool self_close_request;
};

#endif
