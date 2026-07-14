# pude / PUTerm

## Overview

**`pude`** is PureUNIX's ring-3 desktop shell: a real SDL2 fullscreen
window manager (docs/sdl-port.md) that draws a dark desktop with a
top-left **Menu** control. Clicking it opens an application launcher;
selecting an entry spawns a real, chrome-decorated window (title bar,
close button, resize grip) running that app. Typing `pude` at any
BusyBox ash prompt launches it, exactly like any other installed program
(`tools/mkext2.py` stages `/bin/pude.elf` with a plain `pude` symlink,
same as `tty`/`ping`/`neatvi`).

```
# pude
   (fullscreen: a dark desktop, a "Menu" button top-left, and
    initially NO windows open at all)
```

Two apps are registered today:

- **PUTerm** — a real terminal emulator (user/pude_term.c/.h) backed by a
  real PTY (include/pureunix/pty.h) and a real, forked+exec'd BusyBox ash.
- **Calculator** — a real ring-3 GUI calculator (user/pude_calc.c/.h),
  pure userspace arithmetic, no kernel involvement.

Both plug into the same generic window/app abstraction
(`user/pude_app.h`) — `user/pude.c` itself contains **no PUTerm or
Calculator logic whatsoever**, only window list management, chrome
rendering/hit-testing, the launcher, and the top-level SDL event loop.
Multiple windows can be open at once, of the same or different apps
(e.g. two PUTerm windows and a Calculator simultaneously), each fully
independent.

## Architecture: the app_class_t abstraction

`user/pude_app.h` defines the one interface every `pude` application
implements — a `const app_class_t` describing identity, default/minimum
*client* size (the drawable area inside a window's chrome — an app never
needs to know border thickness or title bar height, only its own pixel
canvas), and a small set of lifecycle callbacks:

```c
typedef struct {
    const char *name;
    int default_client_w, default_client_h;
    int min_client_w, min_client_h;

    void *(*create)(pude_window_t *win, int client_w, int client_h);
    void  (*destroy)(pude_window_t *win, void *state);
    void  (*render)(pude_window_t *win, void *state, SDL_Surface *s,
                     int cx, int cy, int cw, int ch);
    void  (*on_key)(pude_window_t *win, void *state, SDL_Scancode sc,
                     key_mods_t mods, bool down);
    void  (*on_mouse_down)(pude_window_t *win, void *state, int x, int y);
    void  (*on_mouse_up)(pude_window_t *win, void *state, int x, int y);
    void  (*on_resize)(pude_window_t *win, void *state, int new_client_w, int new_client_h);
    bool  (*poll)(pude_window_t *win, void *state);
    bool  (*is_alive)(pude_window_t *win, void *state);
} app_class_t;
```

Only `create`/`destroy`/`render` are mandatory — everything else is
NULL-checked by the WM before being called, so an app with no use for
keyboard input (Calculator) or no backing resource to poll (also
Calculator) simply leaves those NULL. `pude_window_t` is the WM-owned
per-instance record: whole-window geometry (chrome included) plus the
app's own opaque `state` pointer returned by `create()`.

**Adding a third app** means: write a `.c`/`.h` implementing this vtable
(see `user/pude_calc.c` for the smallest real example), add a build rule
to the Makefile (pattern-matches `pude_term.o`/`pude_calc.o`'s existing
rules exactly), link its `.o` into `PUDE_ELF`, and add one line to
`user/pude.c`'s `g_apps[]` registry. Nothing else in the WM changes.

## Window manager: `user/pude.c`

Owns a fixed pool of `pude_window_t` (`MAX_WINDOWS` = 12, same
fixed-pool style as `kernel/pty.c`'s `MAX_PTYS`) plus a z-order array
(`win_order`) — index 0 is bottommost, the last entry is topmost/focused.
Only the order array is reshuffled on click-to-front or close; pool slots
never move, so a spawned app's `pude_window_t *` stays valid for its
whole lifetime.

- **Launcher**: a "Menu" button (top-left, fixed position) toggles a
  popup listing every registered app by name (`g_apps[]`). Clicking an
  entry calls `spawn_window()`: allocates a pool slot, computes initial
  whole-window geometry from `cls->default_client_w/h` plus chrome
  (cascading each new window 26px right/down so repeated launches don't
  stack exactly on top of each other), then calls the app's `create()`.
  If `create()` returns NULL (e.g. the pty pool is exhausted) the spawn
  silently aborts.
- **Chrome**: every window gets a border, a title bar (app name, drawn
  brighter blue when focused / dimmed gray otherwise), a close button
  (red square with "X", top-right of the title bar), and a bottom-right
  resize grip — all drawn generically by `draw_window_chrome()`, with the
  app's own `render()` called only for the interior client rect.
- **Focus/z-order**: clicking anywhere inside a window's whole rect
  brings it to front (`bring_to_front()`) before any further hit-testing
  (close/resize/titlebar-drag/client-click) happens against it. Keyboard
  input always goes to `win_order[win_count-1]` (the topmost window).
- **Move**: pressing inside the title bar (excluding the close button)
  starts a drag; motion updates `win->x/y` live, clamped to stay fully
  on screen.
- **Resize**: pressing inside the 22×22 bottom-right grip starts a
  resize; motion updates `win->w/h` live (clamped to the app's
  `min_client_w/h` plus chrome, and to the screen). **On release**, the
  WM computes the new client-area pixel size and calls
  `cls->on_resize(win, state, new_client_w, new_client_h)` exactly
  once — the one general-purpose mechanism every app uses to learn its
  new drawable dimensions and re-layout. This mirrors the original
  single-window `pude`'s behavior (reflow on release, not on every
  motion event).
- **Close**: clicking the close button calls `cls->destroy(win, state)`
  — for PUTerm this actually terminates the shell (`SIGHUP` + `waitpid`)
  and closes its pty; for Calculator it just frees a small struct — then
  removes the window from the pool/order array. Closing one window never
  affects any other open window or the desktop itself.
- **Self-ending apps**: every frame, after calling each window's
  `poll()`, the WM checks `is_alive()` (if non-NULL) and auto-closes any
  window whose app ended on its own (PUTerm: the shell exited via
  `exit`) — exactly like a user-initiated close, never taking the rest
  of the desktop down with it.
- **`Ctrl+F12`**: emergency whole-desktop quit (closes every open window,
  then exits `pude` itself) — an escape hatch for the rare case something
  wedges; the intended, tested path for a single window is its own close
  button, and for the whole desktop is simply closing every window and
  letting the outer shell resume.

## Shared drawing primitives: `user/pude_gfx.h`

Header-only (`static inline`, one copy compiled per translation unit) so
adding a new app never needs a new Makefile object rule just to draw
text or rectangles: `pu_put_pixel`/`pu_fill_rect`/`pu_draw_rect_outline`/
`pu_draw_glyph`/`pu_draw_string`/`pu_draw_string_centered`, plus the
`FONT_CELL_W`/`H` metrics and `font_glyph()` declaration. Reuses
`drivers/font.c`'s exact pre-rasterized Menlo glyph bitmap (the same one
the VGA text console uses) by compiling that source file as its own
object linked into `pude.elf` — it can't be `#include`'d normally from a
newlib translation unit (`include/pureunix/font.h` transitively pulls in
`include/pureunix/types.h`, whose kernel-style `uid_t`/`gid_t`/...
typedefs collide with newlib's own `<sys/types.h>`), so `pude_gfx.h`
forward-declares just the narrow piece it needs instead.

## PUTerm: a real terminal emulator, not a snapshot viewer

`user/pude_term.c`/`.h` is a from-scratch ANSI/VT100-subset
escape-sequence interpreter — a cell grid (`term_cell_t[rows][cols]`,
16-color SGR palette, bold/reverse), a real state machine
(`TS_NORMAL`/`TS_ESC`/`TS_CSI`) for CSI cursor movement/erase/SGR,
`IND`/`RI` (`\ED`/`\EM`), `DECSC`/`DECRC` (`\E7`/`\E8`), and DECSTBM
scroll regions — plus a plain-text scrollback ring, all fed by raw bytes
read from a real pty master. **This is deliberately not a call into
`drivers/vga.c`** (kernel-only, coupled to real VGA hardware state) —
the exact escape-sequence subset matches `drivers/vga.c`'s own capability
set (`third_party/ncurses/pureunix.terminfo` documents it
capability-by-capability), so anything already working against the
physical console (ash, coreutils, Lua, htop, ncurses apps) produces the
same visible result inside PUTerm.

The file has two halves:

1. **The `term_t` engine** (unchanged since the original single-window
   `pude`): `term_init`/`term_feed`/`term_resize`/`term_scroll_view`,
   entirely independent of the WM or SDL.
2. **The `app_class_t` wrapper** (`puterm_app_class`, at the bottom of
   the file): `puterm_create()` allocates a `term_t` **on the heap**
   (`malloc`, ~190 KiB — this kernel has no demand-paged/guard-page stack
   growth, so it can never be a stack local; the original single-window
   `pude` used a `static term_t` precisely because there was only ever
   one instance, but a real incremental `sbrk()`/heap, added during the
   Chocolate Doom port, is what makes more than one concurrent PUTerm
   window possible at all), creates a real pty (`pu_pty_create`), and
   forks+`execve`s `/bin/sh` onto its slave end exactly like a real
   terminal emulator. `puterm_poll()` drains the pty master
   non-blockingly into `term_feed()` and reaps the child with
   `waitpid(WNOHANG)`; `puterm_on_resize()` calls `term_resize()` and
   `ioctl(TIOCSWINSZ)` (which itself signals a real `SIGWINCH` to the
   pty's foreground process group); `puterm_is_alive()` reports whether
   the shell has exited, so the WM can auto-close the window.

Every window spawned from the launcher gets its **own independent**
pty + forked shell + `term_t` — nothing is shared between two open
PUTerm windows (`kernel/pty.c`'s `MAX_PTYS` = 8 is the only ceiling,
plenty for realistic concurrent use).

Keyboard input is the other half of being a real terminal emulator:
`puterm_on_key()` receives plain `SDL_Scancode` + `key_mods_t` from the
WM (physical Shift/Ctrl state is tracked once, globally, by the WM —
no app does its own modifier bookkeeping) and encodes each key into the
byte(s) a real terminal would send, exactly as before — arrow keys/
F-keys/Home/End/etc. as the exact escape sequences
`third_party/ncurses/pureunix.terminfo` and `drivers/tty.c`'s
`key_to_escape_seq()` already use.

## Calculator: `user/pude_calc.c`/`.h`

A small, real ring-3 GUI application — pure userspace arithmetic
(`double` accumulator, pending operator, "entering a new operand" flag,
error state) and pixel rendering, with **no backing process and no
kernel involvement**, unlike PUTerm. Digits 0–9, a decimal point, `+`/
`-`/`*`/`/`, `=`, and `C` (clear), laid out as a 4-column button grid
(a 5th row's single wide "C" button spans the full width) above a
display panel.

Both `render()` and `on_mouse_down()`'s hit-testing recompute the grid
layout (`row_h = (client_h - display_h) / 5`, `col_w = client_w / 4`)
from the window's **current** client size (`calc_state_t.cw/ch`, updated
by `on_resize()`) rather than any fixed pixel resolution — a resize
re-lays out the button grid and re-centers every label
(`pu_draw_string_centered`), it never stretches a fixed-size rendering.
Division by zero sets an error state (display shows `Error`); any button
other than `C` pressed while in that state clears it first, like a real
calculator.

## Testing

`tools/test-pude.py` (rewritten for the multi-window desktop, same QMP
`screendump`-based technique as `tools/test-ncurses-demo.py`): boots the
real `make iso` persistent disk image, types `pude` at a genuine BusyBox
ash prompt, and drives real injected keyboard/mouse input via QMP
`send-key` and HMP `mouse_move`/`mouse_button` (`QemuSession.mouse_rel()`
— QMP's own `input-send-event` "rel"/"btn" types are a confirmed silent
no-op against this environment's real PS/2 mouse). Every mouse move is
split into many small relative steps via `walk_rel()`/`mouse_to()`,
never one large jump — a single oversized `mouse_move` call is itself
clamped/truncated by QEMU, so reaching a precise target (or reliably
clamping into a screen corner via a large overshoot) needs several real
motion events, exactly like a human dragging a physical mouse.

Exercises, all screenshot-verified against real rendered pixels:

- the desktop appearing with **no window auto-opened**, just the
  top-left "Menu" control
- clicking "Menu" opens the launcher popup listing PUTerm and Calculator
- launching PUTerm: real chrome (title bar/border/close button) appears,
  running a real ash prompt; `echo`, `ls /`, and
  `lua -e "print(6*7)"` all produce correct real output
- launching Calculator **while PUTerm is still open** (proving
  multi-window coexistence, with the newly-focused window's title bar
  drawn brighter and the other dimmed)
- `7`, `+`, `8`, `=` clicked via real mouse input correctly computes `15`
- dragging Calculator's resize grip: the button grid visibly re-lays out
  to the new size, and a click afterward still hits the intended button
  (hit-testing stays correct post-relayout)
- dragging PUTerm's resize grip: the terminal reflows to the new pixel
  size and the shell keeps working (`echo` immediately after)
- closing PUTerm via its close button: the window disappears but the
  desktop and Menu control stay fully alive
- reopening PUTerm from the launcher after closing it: a fresh window
  with a fresh, working shell (`echo reopened_ok`)
- `Ctrl+F12` (emergency whole-desktop quit) cleanly returns control to
  the outer ash shell that launched `pude`

**Full regression suite unaffected**: `make run-test` — `systest`
343/345 (the same 2 pre-existing, unrelated console-geometry failures,
`[129]`/`[130]`, see project memory) — plus all 5 other
`tools/vt-scripts/` scripts (`ash-job-control`,
`ctrl-c-foreground-sleep`, `ctrl-quit-foreground-sleep`, `proc-ps-top`,
`vt-switch-does-not-interrupt`) still pass unchanged, confirming the
desktop rework didn't regress any physical-VT-based behavior it shares
code with (font rendering, pty/job-control plumbing, etc).

## Known limitations

- **Only the bottom-right corner resizes** — no edge/other-corner grips,
  matching this project's "keep it simple" scope. Nothing about the
  resize protocol (`on_resize`) assumes this; a future WM feature could
  add more grips without touching any app.
- **Resize-shrink clips rather than reflows** (PUTerm inherits this from
  the original single-window design) — real terminal emulators generally
  do the same; the redrawing program (ash, vi, ...) is what actually
  fixes up the display, exactly like a real xterm resize.
- **Master reads are always non-blocking** (see `include/pureunix/
  pty.h`): PUTerm always wants non-blocking polling from its render/input
  loop, so that's the only pty-master mode implemented.
- **No window minimize/maximize** — only move, resize, and close.
- **`MAX_WINDOWS` = 12, `MAX_PTYS` = 8** — fixed pools (same style as
  `kernel/pty.c`'s existing `MAX_PTYS`); a launch beyond either ceiling
  silently no-ops rather than queuing or erroring visibly.
- **Scrollback has no attributes** (PUTerm, plain text in the default
  color) — the same simplification `kernel/vt.c`'s own scrollback makes.
- **Calculator has no keyboard input** — mouse-only by design (the task
  this app was built for only required mouse-driven buttons); its
  `on_key` is NULL. A future revision wanting numeric-keypad entry would
  just implement that callback.
