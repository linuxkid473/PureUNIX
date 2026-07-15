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

Four apps are registered today:

- **PUTerm** — a real terminal emulator (user/pude_term.c/.h) backed by a
  real PTY (include/pureunix/pty.h) and a real, forked+exec'd BusyBox ash.
- **Calculator** — a real ring-3 GUI calculator (user/pude_calc.c/.h),
  pure userspace arithmetic, no kernel involvement.
- **PUFiles** — a real ring-3 graphical file manager (user/pude_files.c/.h)
  backed by PureUNIX's actual filesystem (see this document's "PUFiles"
  section below).
- **PUText** — a real ring-3 graphical text editor (user/pude_text.c/.h)
  with a dynamic document buffer, real Open/Save/Save-As, clipboard, and
  mouse/keyboard selection (see this document's "PUText" section below).

All four plug into the same generic window/app abstraction
(`user/pude_app.h`) — `user/pude.c` itself contains **no PUTerm,
Calculator, PUFiles, or PUText logic whatsoever**, only window list
management, chrome rendering/hit-testing, the launcher, and the top-level
SDL event loop. Multiple windows can be open at once, of the same or
different apps (e.g. two PUTerm windows and a Calculator simultaneously,
or two independent PUText documents), each fully independent.

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
    void  (*on_mouse_move)(pude_window_t *win, void *state, int x, int y);
    void  (*on_resize)(pude_window_t *win, void *state, int new_client_w, int new_client_h);
    bool  (*poll)(pude_window_t *win, void *state);
    bool  (*is_alive)(pude_window_t *win, void *state);
    bool  (*confirm_close)(pude_window_t *win, void *state);
} app_class_t;

struct pude_window {
    const app_class_t *cls;
    void *state;
    char title[48];
    int x, y, w, h;
    bool self_close_request; /* app-settable; see confirm_close below */
};
```

Only `create`/`destroy`/`render` are mandatory — everything else is
NULL-checked by the WM before being called, so an app with no use for
keyboard input (Calculator) or no backing resource to poll (also
Calculator) simply leaves those NULL. `pude_window_t` is the WM-owned
per-instance record: whole-window geometry (chrome included) plus the
app's own opaque `state` pointer returned by `create()`.

**`on_mouse_move`** (added for PUText's click-and-drag text selection —
PUFiles' own click-only toolbar/list interactions never needed it) is
forwarded on every mouse-motion event for as long as the left button is
held down *and* it was originally pressed inside this same window's
client area — never called while the WM itself is using the drag for
window move/resize. Coordinates are client-relative like `on_mouse_down`/
`on_mouse_up` but not clamped to the client rect (a drag can go past a
window's own edge); an app that cares clamps itself.

**`confirm_close`** (added for PUText's unsaved-change protection) is an
optional gate on the close button — `NULL` keeps every existing app's old
behavior (close immediately). Returning `true` lets the WM close the
window right away, same as before; returning `false` tells the WM to
leave the window open, and the app is expected to have reacted by
switching into its own in-window confirmation modal as a side effect of
that same call (PUText's "Discard unsaved changes?"). There is no second
callback for "the user confirmed" — an app that wants to actually close
after the user picks Discard sets **`win->self_close_request = true`**
from whatever click/key handler resolves that modal; the WM notices it on
its very next frame and closes the window, bypassing `confirm_close`
entirely (it already did its job). This mirrors `pude_spawn.h`'s existing
"app requests, WM acts" pattern rather than letting an app mutate the
window pool/z-order itself.

**Adding another app** means: write a `.c`/`.h` implementing this vtable
(see `user/pude_calc.c` for the smallest real example), add a build rule
to the Makefile (pattern-matches `pude_term.o`/`pude_calc.o`/
`pude_files.o`/`pude_text.o`'s existing rules exactly), link its `.o` into
`PUDE_ELF`, and add one line to `user/pude.c`'s `g_apps[]` registry.
Nothing else in the WM changes.

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

Also has `pu_draw_string_clipped(s, x0, y0, max_w, str, fg, bg)` — draws
at most as many glyphs as fit within `max_w` pixels, truncating with a
trailing `"..."` if the string doesn't fit. `pu_draw_string`/
`pu_draw_string_centered` only clip per-pixel against the *whole screen
surface* (`pu_put_pixel`'s own bounds check), not any narrower rectangle
— fine for PUTerm (bounded by its own cell grid) and Calculator (short,
fixed button labels), but not for PUFiles, which draws arbitrary-length,
filesystem-controlled text (file names, full paths) that must never
bleed past its own window's edge into a neighboring window or the
desktop. Added while building PUFiles; `pu_button_draw` (below) uses it
too, for the same reason.

## Shared widgets: `user/pude_widgets.h`

Header-only, same convention as `pude_gfx.h`, added while building
PUFiles once it became clear more than one app needed the same small
pieces:

- `pu_scancode_to_ascii(sc, mods)` — plain scancode→character mapping (no
  escape sequences, unlike PUTerm's own terminal byte encoder) for typing
  into a text field. PUTerm's `encode_key()` now calls this too instead
  of keeping its own private copy of the same lookup table.
- `pu_button_t` / `pu_button_hit()` / `pu_button_draw()` — a labeled
  rectangle: hit-testing and drawing, no state of its own. Used by
  PUFiles' toolbar and its New Folder/Rename/Delete dialogs.
- `pu_textinput_t` / `pu_textinput_set/putc/backspace/draw()` — a
  single-line text field that only appends/erases at the end (no
  mid-string cursor placement — every current caller only ever needs to
  type or correct a short name). Used by PUFiles' New Folder/Rename
  dialogs.
- `pu_list_visible_rows()` / `pu_list_row_at()` — trivial scrollable-list
  arithmetic (how many rows fit, which row a click lands on) that owns no
  item data itself, since every real caller's row content differs
  completely.

Deliberately not a full GUI toolkit: no layout engine, no widget tree, no
event dispatch of its own — just the handful of pieces that were about to
be duplicated a second time. Add more only when a third real app needs
the same thing again.

## Reusable file picker: `user/pude_filepicker.h`

Added while building PUText, which needed a real graphical Open and
Save-As flow and found nothing reusable: PUFiles (`user/pude_files.c`) is
a whole `app_class_t`, not something another app can embed, and
`pu_textinput_t` has no directory listing/navigation of its own. Rather
than duplicate PUFiles' browsing logic a second time inside PUText, this
is a small, generic, embeddable widget — header-only (`static inline`),
same convention as `pude_gfx.h`/`pude_widgets.h` — that any app can drop
into its own state struct and draw/hit-test as a centered modal over its
own client area (the same convention PUFiles' own New Folder/Rename/
Delete dialogs already use, taken one step further into a real directory
browser).

`pu_filepicker_t` owns: the current directory, a real `opendir()`/
`readdir()`/`stat()`-backed entry listing (dirs-first, case-insensitive —
same sort as PUFiles' `pf_compare()`), a selection/scroll position, and
(Save mode only) a `pu_textinput_t` for the filename. Two constructors —
`pu_filepicker_open_init()` / `pu_filepicker_save_init()` — pick the mode;
`pu_filepicker_on_mouse_down()` / `pu_filepicker_on_key()` handle
navigation (Up button, double-click/Enter into a directory, Home/End/
PageUp/PageDown/arrow-key selection, typing into the filename field in
Save mode) and return a `pu_fp_result_t` (`PU_FP_NONE` /
`PU_FP_CONFIRMED` / `PU_FP_CANCELLED`) the embedding app checks after
every call; `pu_filepicker_draw()` renders it; `pu_filepicker_result_path()`
joins the current directory with the selected entry (Open) or typed
filename (Save) into the caller's own buffer. There is no filesystem
mutation anywhere in this widget — it only ever reads a directory and
hands back a path string, exactly the boundary PUText's own Open/Save
logic (real `fopen()`/`fwrite()`) needs on the other side of it.

Like `pude_widgets.h`, deliberately not a full toolkit: no drag-to-resize,
no breadcrumbs, no favorites/recents list — just enough to browse to a
file or type a new filename, which is the whole job Save-As and Open need
done.

## Cross-app clipboard: `user/pude_clipboard.h`/`.c`

A tiny desktop-session-local, text-only clipboard shared by every `pude`
app — the general mechanism PUText's Ctrl+C/X/V need, built the same way
`pude_spawn.h`'s cross-app mailbox was: real shared mutable state in its
own `.c` file (not header-only), because this genuinely is shared state
between translation units, not a stateless helper. `pude_clipboard_set(text,
len)` replaces the clipboard's contents (freeing the old buffer);
`pude_clipboard_get(&len)` returns a pointer to the current contents (valid
until the next `set()`, so a caller that needs to keep it copies it out);
`pude_clipboard_has_data()` reports whether anything is stored.

"Desktop-session-local" means the data lives in `pude`'s own process
memory for as long as it's running — there is no cross-reboot persistence
and no host-OS clipboard integration (this environment has neither),
exactly like every other cross-window mechanism in this desktop
(`pude_spawn.h`'s mailbox, the window pool itself). Because it's real
shared state in one process's address space (not private per-instance
state inside PUText), Ctrl+C in one PUText window and Ctrl+V in a
*different* PUText window round-trips correctly today, and any future app
(PUTerm selection, PUFiles multi-select copy/paste, ...) can read/write
the same clipboard without PUText knowing anything about it.

## Launching a foreground program: `user/pude_launch.c`/`.h`

Some files PUFiles opens (see below) need a real external *fullscreen*
program, not another chrome-decorated window — reusing the existing
native PNG viewer, `imgview` (docs/imgview.md), rather than duplicating
its libpng decoding inside PUFiles. `imgview` itself calls
`pu_set_graphics_mode(1)` and draws straight to the hardware framebuffer
via `pu_fb_mmap()`/`pu_fb_blit()`, and reads keyboard/mouse input directly
via `pu_input_poll()` — exactly like `pude` itself does through SDL2.
Naively `fork()`+`execve()`ing it from inside a PUFiles window while
`pude`'s own main loop kept running would race it for that same input
queue (SDL's `PUREUNIX_PumpEvents()` calls the identical `pu_input_poll()`
every frame) and periodically paint `pude`'s own stale desktop surface
right over imgview's freshly rendered frame.

`pude_launch_foreground(path, argv)` is the smallest correct fix:
`fork()`s, `execve()`s the target, and then **blocks the entire calling
process** in a real `waitpid()` until the child exits — no `SDL_PollEvent`
calls happen at all while it runs, so neither race is possible. This
costs nothing extra: a fullscreen program legitimately owns the whole
display until it exits, so there's nothing useful left for any other
`pude` window to be doing on screen in the meantime anyway. It also needs
no new kernel mechanism — `kernel/vt.c`'s `graphics_owner_stack` (added
for Chocolate Doom, see this document's own history) already handles a
child pushing a nested `SYS_SET_GRAPHICS_MODE(1)` and popping back to the
parent's ownership on exit; the only actual gap was pude's own *process*
still polling input/repainting during that window, which blocking closes.
After the child exits, control returns to whatever `on_mouse_down`/
`on_key` callback made the call, already inside `pude.c`'s own per-frame
event handling, so the very next iteration's `need_redraw` naturally
repaints the whole desktop fresh.

## Cross-app window spawning: `user/pude_spawn.h`/`.c`

A tiny one-slot mailbox letting an app ask the WM to open a *new window*
of a different app class — e.g. PUFiles asking for a fresh PUTerm window
preloaded with an editor command. Only `user/pude.c`'s own
`spawn_window()` may allocate a window-pool slot/z-order entry, so an app
can't do this directly; `pude_request_spawn(cls, startup_command)` queues
a request, and `pude.c`'s main loop drains it once per frame via
`pude_take_spawn_request()`, calling `puterm_set_startup_command()` first
if the target is PUTerm. Real shared mutable state across two
translation units, so (unlike `pude_gfx.h`/`pude_widgets.h`) it needs an
actual `.c` file, not a header-only `static`.

`puterm_set_startup_command(cmd)` (`user/pude_term.h`) queues a command
to be typed into the *next* PUTerm window's shell the instant it's
created, exactly as if the user had typed it themselves — consumed and
cleared by that window's own `puterm_create()`. This is genuinely just
"type ahead into a fresh pty", nothing PUTerm-specific about the
mechanism: `puterm_create()` writes `cmd + "\n"` into the master fd right
after forking the shell, before the WM ever calls `render()` on it.

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

## PUFiles: a real graphical file manager

`user/pude_files.c`/`.h` is a real ring-3 file manager backed entirely by
PureUNIX's actual VFS/EXT2/FAT16 filesystem — every directory listing,
navigation, create/rename/delete operation goes through ordinary newlib
calls (`opendir`/`readdir`/`closedir`/`stat`/`mkdir`/`rmdir`/`unlink`/
`rename`, `user/newlib_syscalls.c`) exactly as any real userspace program
would. There is no mock directory tree, hardcoded listing, or kernel-side
GUI logic anywhere in it. Starts at `/`.

**Layout** (top to bottom): a path bar showing the current directory, a
toolbar (Up / New Folder / Rename / Delete / Refresh — Up/Rename/Delete
gray themselves out when meaningless, e.g. Rename with nothing selected),
a scrollable entry list, and a status/error bar. All four regions and the
button grid recompute from the window's *current* client size on every
`render()`/`on_resize()`, the same "never stretch a fixed-size rendering"
rule Calculator's button grid follows.

**Listing a directory** (`pf_load_dir()`): one `opendir()`/`readdir()`
loop per navigation, skipping `.` always and `..` only at `/` (nothing
above root to navigate to). `readdir()`'s own `d_type` (DT_DIR/DT_REG/
DT_LNK) is enough to tell files from directories for free; a `stat()` per
non-directory entry additionally gets its real size, and a `stat()` (not
`lstat()`) per symlink decides whether it *behaves* like a directory for
navigation purposes (a broken link just stays a plain, non-navigable
entry — opening it later reports that real failure). Entries sort
directories-first (`..` always pinned above everything else), then
case-insensitively alphabetical within each group — a `qsort()` over the
one comparator `pf_compare()`, not a stable/insertion-order listing.

**Navigation**: double-click or Enter opens the selected entry; Backspace
or the Up button goes to the parent (`..`'s own row does the same via
double-click/Enter). Up/Down/PageUp/PageDown/Home/End move the selection
and keep it scrolled into view (`pf_ensure_visible()`); there is no mouse
wheel or scrollbar-thumb drag on this platform at all (`pu_input_event_t`
has no scroll event, and `app_class_t` has no mouse-motion callback for a
drag-to-scroll gesture), so keyboard scrolling plus letting a newly
created/renamed entry auto-scroll into view is the whole scrolling story
in this first version.

**Opening a file** (`pf_open_entry()`) is one small, general dispatch,
not per-extension special casing sprinkled around:
- a directory navigates in-place (or up, for `..`);
- a `.png` (case-insensitive) hands the whole screen to the real,
  unmodified `imgview` via `pude_launch_foreground()` — see that
  section above; no PNG decoding duplicated here;
- a file with **any** execute permission bit set is refused outright
  (`"refusing to run an executable file"`, shown in the status bar) —
  selecting a file must never execute it, full stop;
- anything else is assumed to be plain text and opened in a brand-new
  **PUText** window (`pude_request_spawn(&putext_app_class, full)`, this
  document's "PUText" section below) — covers `.txt`/`.md`/`.c`/`.h`/
  `.lua`/`.conf`/`.cfg`/`.ini`/`.sh`/anything else that isn't flagged
  executable, the desired "double-click a text file → PUText opens it
  already loaded" flow. (Before PUText existed, this same bucket opened
  `neatvi` in a fresh PUTerm window instead — same dispatch rule, just a
  real graphical editor on the other end of it now.)

**Create/rename/delete** all go through a small, reusable modal built
from `pude_widgets.h`'s button/text-input primitives (drawn centered over
the window, not a separate WM-level dialog concept):
- **New Folder** / **Rename** open a text-input dialog (pre-filled with
  the current name for Rename); OK calls `mkdir()`/`rename()` and
  re-selects the created/renamed entry; Cancel/Escape aborts with no
  filesystem change.
- **Delete** always shows a Cancel/Delete confirmation dialog first
  (`"Delete 'name'? This cannot be undone."`) — no direct delete path
  exists. A regular file calls `unlink()`; a directory calls `rmdir()`,
  which is **not** recursive in this first version — deleting a
  non-empty directory correctly fails with a real `ENOTEMPTY`
  (`fs/ext2/write.c` already enforces this), shown in red in the status
  bar exactly like any other error, not silently ignored or only logged
  to the serial console.
- Every operation's real result — success or a real `errno` via
  `strerror()` (permission denied, path too long, name too long, no such
  file, directory not empty, ...) — is shown in that same in-window
  status bar, never only printed to the serial console.

All name/path buffers are sized to this kernel's own real limits
(`PUREUNIX_MAX_NAME` = 64, `PUREUNIX_MAX_PATH` = 256, `include/pureunix/
config.h`) and every path built with `snprintf()` checked for truncation
before use; every drawn string (path bar, list rows, dialog text) goes
through `pu_draw_string_clipped()` (see `pude_gfx.h` above), so a name or
path longer than the window is wide is truncated with `"..."` on screen
rather than overflowing into a neighboring window — a too-long name is
still rejected outright by `mkdir()`/`rename()` with a real
`ENAMETOOLONG`.

### A real bug this app found: newlib's `rename()` didn't exist

Renaming a **directory** through PUFiles' own Rename dialog initially
failed with a confusing `EPERM` ("Not owner"). The real cause: no
newlib-facing `rename()` had ever been defined in `user/
newlib_syscalls.c` (only the lower-level `pu_rename()` in `libpure.h`,
used by non-newlib programs like `systest`/`ext2test`) — any newlib-linked
program calling the standard POSIX `rename()` silently fell back to
newlib's own generic implementation, `link()` the new name then
`unlink()` the old one. That "worked" for plain files, but `ext2_link()`
(`fs/ext2/write.c`) correctly refuses to hard-link a directory, which is
exactly what that fallback tried first. Fixed with the smallest correct
change: a real `rename()` wrapper in `user/newlib_syscalls.c` calling
`PU_SYS_RENAME` directly, exactly like the existing `unlink()`/`mkdir()`/
`rmdir()` wrappers already do — `vfs_rename()`/`ext2_rename()`
(`fs/vfs.c`, `fs/ext2/write.c`) already implement a real, atomic,
directory-safe rename; it just had never been reachable from any
newlib-linked program (`pude`, PUTerm's shell, BusyBox, ...) before this.

## PUText: a real graphical text editor

`user/pude_text.c`/`.h` is a real ring-3 text editor, plugged into the WM
through the same `app_class_t` PUTerm/Calculator/PUFiles use. It replaces
PUFiles' previous default of opening a plain text file with `neatvi`
inside a fresh PUTerm window (see "Opening files" below) — the same
"assumed to be text, not an extension allowlist" dispatch rule as before,
just handed to a real graphical editor now instead of a terminal one.

### Document buffer

`pt_doc_t` is a dynamic array of dynamically-grown lines (`pt_line_t`) —
deliberately not `neatvi`'s `lbuf.c` (inspected for ideas, not embedded:
that structure is built around `ex`/regex/undo machinery this editor
doesn't need) and deliberately not a fixed-size buffer. Each line grows
via `realloc()` doubling with no built-in ceiling on length; the line
array itself grows the same way, so there is no built-in ceiling on
document size either, beyond real available heap — the ring-3 `sbrk()`
heap (`include/pureunix/vmm.h`) is a real, incrementally-grown 32 MiB
region (added during the Chocolate Doom port), more than enough for any
reasonable text file with nothing PUText-specific needed to reach it.

Core operations, all working directly on byte offsets within lines:
- `pt_doc_split_line(doc, row, col)` / `pt_doc_join_line(doc, row)` — the
  two primitives Enter and Backspace-at-column-0/Delete-at-end-of-line
  are built from; split creates a new line holding the tail past `col`,
  join appends the next line onto the end of this one and removes it.
- `pt_doc_delete_range(doc, r0,c0,r1,c1)` — deletes an arbitrary
  (possibly multi-line) range in one call: same-line is a simple
  substring delete, multi-line truncates the first line at `c0`, appends
  the last line's tail past `c1`, and removes every line strictly between.
- `pt_doc_insert_text(doc, row, col, text, len)` — inserts arbitrary text
  that may contain embedded `'\n'`s, splitting lines as needed; the one
  primitive both typed-Enter (a single `'\n'`) and a multi-line paste
  share.
- `pt_doc_get_range(doc, r0,c0,r1,c1)` — extracts a range as one malloc'd
  buffer (embedded lines joined with `'\n'`) for Copy/Cut.

**Load/save and the trailing-newline edge case**: `pt_doc_load()` reads
the whole file, then splits on `'\n'` — an empty file becomes one empty
line; a file whose last byte isn't `'\n'` becomes lines with no implied
trailing one; any number of consecutive `'\n'`s becomes that many empty
lines; all uniformly, no special-casing. A `trailing_newline` flag
(learned from whether the *last* byte of the loaded file was `'\n'`,
`false` for a brand-new document) is reproduced exactly on save, so a
fresh empty document saves as a real 0-byte file, a file loaded without a
trailing newline round-trips without gaining one, and a normal file
round-trips byte-for-byte.

### Cursor and selection model

`(row, col)` byte-offset pairs. A selection is just an anchor `(row,col)`
plus the live cursor — normalized on demand via `pt_sel_range()` (returns
`(r0,c0) <= (r1,c1)`), not a separate object kept in sync. Every cursor
movement (arrow keys, Home/End, Page Up/Down, mouse click/drag) goes
through one function, `pt_move_cursor_to()`, which starts a new anchor
the first time Shift is held (and only then), so keyboard and mouse
selection share exactly the same extend/collapse logic — there is no
separate "mouse selection" code path from "keyboard selection". Vertical
movement (Up/Down/Page Up/Down) tracks a `desired_col` ("goal column")
separately from the live cursor column, so moving down through a short
line and back up doesn't lose the original horizontal position — standard
editor behavior, cheap to get right once the anchor/cursor split already
exists.

Mouse click-and-drag selection needed one real, general WM change:
`app_class_t` had no mouse-motion callback at all before this (PUFiles'
own interactions are all single-click; see `pude_app.h`'s `on_mouse_move`
in the architecture section above) — added rather than worked around,
since a text editor without drag-to-select would not be a real editor.

### Scrolling

`scroll_top` (first visible line) and `scroll_col` (first visible
column, in characters — the font is fixed-width, so this is exact, no
sub-character scrolling to reason about) are kept in sync with the cursor
by `pt_ensure_visible()`, called after every edit and every cursor move:
clamps `scroll_top`/`scroll_col` to bring the live cursor back into the
visible `rows × cols` window whenever it would otherwise leave it,
exactly like PUFiles' own `pf_ensure_visible()`. A resize
(`putext_on_resize`) recomputes visible rows/cols from the new client
size and re-clamps — the same "never stretch a fixed-size rendering, only
re-lay-out" rule Calculator's button grid and PUFiles' list already
follow.

### Clipboard

Ctrl+C/X/V go through `user/pude_clipboard.h`'s real cross-app clipboard
(see that section above), not private per-window state — copying in one
PUText window and pasting in a second, independent PUText window works
today, and any future app can read/write the same clipboard.

### File picker (Open / Save As)

Both flows use the embeddable `user/pude_filepicker.h` widget (see that
section above), drawn as a modal centered over PUText's own client area —
never a hardcoded path. **Open**: if the current document has unsaved
changes, a "Discard unsaved changes?" confirm modal appears first (see
"Unsaved-change protection" below); otherwise the picker opens directly
in Open mode rooted at the current file's directory (or `/` for a new
document). Selecting a real file (double-click, or Enter with something
selected) loads it and resets cursor/scroll/selection/modified state.
**Save**: `Ctrl+S`/the Save button writes directly to the already-known
path if the document has one; a brand-new Untitled document (or explicit
**Save As** / `Ctrl+Shift+S`) opens the same picker in Save mode instead,
pre-filled with the current filename if any, with a filename text field
alongside the directory listing — typing a name and clicking Save (or
pressing Enter) writes there. There is no overwrite-confirmation dialog
in this first version (a known limitation, see below) — Save-As onto an
existing name just overwrites it, same as many simple editors.

### Unsaved-change protection

Two independent paths, both driven by the same `st->modified` flag (set
by every editing operation, cleared by a successful Open/Save):
**New/Open** check it directly and switch into an in-window "Discard
unsaved changes?" modal before proceeding if it's set (Cancel returns to
editing untouched, Discard proceeds with New or Open). **Closing the
window** uses the general `confirm_close` mechanism added to
`app_class_t` for exactly this (see the architecture section above):
`putext_confirm_close()` returns `true` immediately (close now, no modal)
if the document is unmodified, or switches into the same confirm modal
and returns `false` (leave the window open) if it isn't — clicking
"Discard & Close" inside that modal sets `win->self_close_request = true`,
which the WM notices on its next frame and closes for real. Both a
just-saved (unmodified) window closing with no modal at all, and a
modified window's Cancel/Discard both working correctly, are exercised by
`tools/test-putext.py`.

### Performance: a solid, not blinking, caret

This tree has no damage-rect compositor — `user/pude.c`'s `render_frame()`
redraws the whole desktop surface whenever anything changes; every other
app in this desktop already only causes a redraw in response to real
input (PUTerm's `poll()` redraws only when the pty actually produced
output; Calculator and PUFiles have no `poll()` at all). A traditional
*blinking* caret would need a periodic timer forcing a full-desktop
repaint roughly twice a second for as long as any PUText window stayed
open, purely to toggle a caret — exactly the kind of needless redraw
traffic this project has fought before (see the framebuffer/cursor-lag
project history). PUText's caret is deliberately **solid, not blinking**:
it's still a real, visible, correctly-positioned text cursor (satisfying
the actual requirement), but it sidesteps the performance question by
construction rather than by adding a WM-wide damage-tracking system this
one app doesn't otherwise need — zero *additional* redraw traffic beyond
what typing/moving/scrolling already causes. Building a real damage-rect
compositor for the whole WM remains open future work if a future app
genuinely needs partial-frame redraws; nothing about PUText's own
rendering (`render()` is already scoped to its own `cx,cy,cw,ch` client
rect by the existing `app_class_t` contract) would need to change if one
is added later.

### Testing: `tools/test-putext.py`

Same QMP/HMP injection technique as `tools/test-pude.py`/
`tools/test-pufiles.py`, against a scratch copy of the real `make iso`
disk image. Exercises, screenshot- and/or real-file-content-verified:
launching PUText from the launcher; typing multiple lines; moving the
cursor into previously-typed text (Home/Up/Up); inserting text in the
middle of a line (arrow-key navigation + typing); deleting text
(Backspace); selecting text with the keyboard (Home, Shift+End); Copy/
paste elsewhere in the document; selecting text with the mouse (click +
drag); Cut then immediately Paste back at the same spot (verified via the
invariant that this must reproduce the exact original document,
regardless of exactly which on-screen region the drag landed on — this
environment's synthetic PS/2 mouse motion isn't pixel-exact, so the test
doesn't lean on that precision anywhere); saving a brand-new Untitled
document through the graphical Save-As picker; closing PUText; a second,
independent PUText window/document open at the same time as the first;
unsaved-change protection (a modified window's close button shows a real
modal; an unmodified one closes immediately with none); opening a real
text file from PUFiles (keyboard-navigated to bring an off-screen row
into view first, since PUFiles has no scrollbar/mouse-wheel — see its own
known limitations) and confirming PUText launches with it already loaded;
resizing the reopened window and confirming it stays correct (typing one
more character post-resize); and, the strongest check, the exact expected
file content verified via a real BusyBox ash `cat` both in the same boot
and after a completely separate second QEMU process reboots the same
on-disk image (real persistence, not just "the process kept running" —
same technique `tools/test-pufiles.py` uses). One real, unrelated project
gotcha rediscovered while writing this test: `drivers/vga.c`'s always-on
scroll-perf reporter can splice `PERF scroll ...` lines into `cat`'s own
output on this shared serial console; the test runs `clear` first and
filters any such lines before comparing content, rather than asserting an
exact contiguous transcript match.

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

`tools/test-pufiles.py` (new, same QMP/HMP injection technique as
`tools/test-pude.py`, plus `tools/test-imgview.py`'s "extract the real
`make -n -W` recipe" trick for building a scratch disk with one added PNG
fixture at `/pngtest.png`): boots the scratch disk, captures a real
`ls -la /` from the outer ash shell as ground truth for exactly where
each entry sorts in PUFiles' own listing (so the script never has to
guess a row's on-screen position), then drives PUFiles end to end —
launching it from the launcher; opening a real multi-file directory
(`/testdir`) and a real text file (`alpha.txt`) into a fresh PUText
window (this document's "PUText" section — the file-association path
`tools/test-putext.py` also exercises in more depth); Up-button parent
navigation; `mkdir()`-ing a real
folder and opening it with a real mouse double-click (correctly showing
it as empty); creating 16 more real subfolders to force list overflow
and scrolling it back to the top via the keyboard; drag-resizing the
window and confirming more rows become visible; renaming a real folder
(the operation that caught the real `rename()` bug above); deleting a
real empty folder after a real confirmation dialog; confirming a
non-empty folder's deletion visibly fails with `ENOTEMPTY`; opening a
real `.png` (fullscreen `imgview` takeover, then a clean, uncorrupted
return to the desktop); opening Calculator alongside PUFiles; closing
and reopening PUFiles from the launcher; and finally confirming the
renamed folder is visible both from the real outer ash shell in the same
boot and from a **second, fully independent QEMU process** booting the
identical on-disk image (real reboot persistence, not merely "the same
process kept running"). All screenshot-verified; the two ash checks are
also asserted against the real serial transcript.

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
- **PUFiles has no scrollbar or mouse-wheel scrolling** — this platform's
  input model has no scroll event (`pu_input_event_t`); Up/Down/PageUp/
  PageDown/Home/End on the keyboard are the only way to scroll a long
  listing. (`app_class_t` gained a mouse-motion callback, `on_mouse_move`,
  for PUText's click-and-drag text selection — but nothing about PUFiles'
  own click-only list interaction uses it, and drag-to-scroll would still
  need a real scroll gesture or a scrollbar thumb neither exists yet.)
- **PUFiles' delete is never recursive** — `rmdir()` on a non-empty
  directory correctly fails visibly (`ENOTEMPTY`) rather than deleting
  its contents; deleting a whole tree needs deleting its contents first,
  by design for this first version.
- **PUFiles' text-input widget has no mid-string cursor** — typing only
  appends, Backspace only removes the last character (`pude_widgets.h`'s
  `pu_textinput_t`); enough for naming/renaming a file, not a general
  text-editing widget.
- **A `pude_launch_foreground()` call (opening a `.png`) blocks the whole
  desktop**, not just the calling window — every other open window stops
  updating until the external program exits. This is intentional (see
  this document's own section on it above), not a bug: a fullscreen
  program legitimately owns the entire display until it exits.
- **PUText's caret is solid, not blinking** — a deliberate choice, not an
  oversight; see this document's "PUText" section for the reasoning
  (there is no damage-rect compositor in this tree, so a blinking caret
  would mean a periodic full-desktop repaint for as long as any PUText
  window stayed open).
- **PUText assumes single-byte/ASCII text** — cursor columns and file
  I/O are plain byte offsets; a valid UTF-8 file loads and saves its
  bytes correctly (nothing mutates them), but multi-byte characters
  render and count as multiple single-byte glyph cells rather than one
  visually-correct character, the same limitation `drivers/vga.c`'s own
  console and PUTerm's cell grid already have.
- **No word-wrap** — a long line simply scrolls horizontally
  (`scroll_col`), it never breaks onto a second visual row.
- **Tab inserts 4 literal spaces**, not a real tab stop — there is no
  tab-aware column math anywhere in the renderer; simplest correct choice
  given the fixed-width font already in use everywhere else in this
  desktop.
- **No undo/redo, no syntax highlighting, no find/replace** — out of
  scope for this first version; nothing about the document buffer
  (`pt_doc_t`) or cursor/selection model would need to change to add any
  of these later (undo in particular would layer naturally on top of the
  existing insert/delete/split/join primitives).
- **Save-As has no overwrite confirmation** — typing the name of a file
  that already exists and clicking Save just overwrites it, same as many
  simple editors; a known, deliberate scope cut, not a bug.
- **`pude_filepicker.h` has no create-new-folder** (unlike PUFiles' own
  New Folder dialog) — Save-As can only save into a directory that
  already exists; navigate to/create the target directory in PUFiles
  first if it doesn't.
