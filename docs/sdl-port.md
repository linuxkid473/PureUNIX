# SDL2 Port

## Overview

Real, unmodified-core [SDL2](https://www.libsdl.org) 2.28.5 runs natively
on PureUNIX: `/bin/sdltest.elf` opens a real fullscreen window backed by
the actual hardware/QEMU framebuffer, renders an animated software surface
via `SDL_UpdateWindowSurface()`, reacts to real PS/2/USB keyboard and PS/2
mouse input delivered through a new kernel event path, times itself with
`SDL_GetTicks()`/`SDL_Delay()` backed by a new syscall, and exits cleanly
back to a fully-restored BusyBox ash shell. This is the platform SDL2
needed to exist at all before something like Chocolate Doom could ever run
on PureUNIX — that's the reason this exists, not just a demo.

```
# /bin/sdltest.elf
   (fullscreen window: a bouncing colored box, reacting to arrow
    keys and the mouse, on the real framebuffer)
# (Escape quits -- shell prompt reappears exactly as it was)
```

Verified against the real, exact `make iso` deliverable (`build/pureunix.iso`,
a genuine MBR+GRUB+EXT2 persistent disk image), not just the ephemeral
ramdisk build — see Testing.

---

## Architecture

### Real upstream source, a new platform backend, no upstream patches

`third_party/SDL2/SDL2-2.28.5/` is an unmodified extraction of the
upstream release tarball (see that directory's `README.md` for the exact
vendoring rationale and the "why not SDL's own build system" reasoning —
same "vendor upstream source, compile it with our own Makefile rules, no
upstream configure/CMake" pattern as TCC/Lua/SQLite/ncurses/htop).

Every PureUNIX-specific decision lives in files this port *adds*, following
SDL's own documented extension points — the same mechanism every real SDL2
platform port (PSP, Vita, N3DS, ...) uses, not a workaround:

- **`include/SDL_config_pureunix.h`** (new) — this platform's `SDL_config.h`,
  hand-written the way SDL's own `SDL_config_minimal.h`/`SDL_config_windows.h`
  templates are designed to be, since there's no `configure`/CMake run to
  generate one. Declares a real libc (`HAVE_LIBC` + the individual
  `HAVE_SIN`/`HAVE_MALLOC`/etc. flags — PureUNIX links a real vendored
  newlib, unlike a truly libc-less target) and cleanly disables what isn't
  implemented yet (see "Known gaps").
- **`src/video/pureunix/`** (new) — the real video/event/timer-adjacent
  backend: `SDL_puvideo.c` (`VideoInit`/`CreateWindow`/`SetDisplayMode`),
  `SDL_pufb.c` (`CreateWindowFramebuffer`/`UpdateWindowFramebuffer`, the
  window-surface path), `SDL_puevents.c` (the real event pump).
- **`src/timer/pureunix/SDL_systimer.c`** (new) — real `SDL_GetTicks64()`/
  `SDL_Delay()`, backed by a new syscall (below), not `SDL_TIMERS_DISABLED`.
- A handful of one-line, mechanical registrations in existing upstream
  files that every platform port must touch (there's no `#ifdef`-free way
  around this) — `SDL_platform.h` (`__PUREUNIX__` detection),
  `SDL_config.h` (dispatch to `SDL_config_pureunix.h`), `SDL_video.c`
  (`PUREUNIX_bootstrap` in the driver list, alongside ~25 existing
  entries), `SDL_sysvideo.h` (its `extern` declaration), `SDL.c`
  (`SDL_GetPlatform()`'s name), `SDL_dynapi.h` (`SDL_DYNAMIC_API 0` — see
  below). None of these touch any *behavior* of an existing platform.

### Why `SDL_DYNAMIC_API` had to be explicitly disabled

SDL2's dynamic-API jump-table system defaults to *on* unless a platform
explicitly opts out (every static/console target — PS2, PSP, Vita, N3DS,
NGAGE — does). Missing this the first time around produced real, confusing
link errors (every public symbol built as `SDL_Init_REAL` etc., with no
`SDL_dynapi.c` providing the plain-named trampolines, since this port
never compiles that file) — fixed by adding a `__pureunix__` case
alongside the existing platform list in `SDL_dynapi.h`. Meaningless for a
target with no dynamic loader at all (`SDL_LOADSO_DISABLED`) — this only
existed to let a *different* prebuilt SDL2 be swapped in at runtime,
which has no PureUNIX equivalent.

### The syscalls this needed (`include/pureunix/syscall.h`)

Dedicated syscalls, not a new `/dev/fb`+`/dev/input` VFS device pair —
every one of these is a one-shot, fixed-shape operation (poll one event,
blit one whole frame, read one clock) with nothing to `open()`/`close()`,
the same reasoning `SYS_PING` already established:

- **`SYS_INPUT_POLL`** — non-blocking pop from the calling task's own VT's
  new *raw* input queue (`include/pureunix/input.h`, `kernel/vt.c`'s
  `vt_raw_input_push_key()`/`_mouse_motion()`/`_mouse_button()`) — a
  second event source alongside the pre-existing ASCII `vt_input_push()`
  queue tty/ash/ncurses use, not a replacement for it. Carries real
  key-down *and* key-up events (the ASCII queue only ever reports
  presses) with a shift-independent key identity, plus mouse motion/button
  events.
- **`SYS_FB_GETINFO`** / **`SYS_FB_BLIT`** — real framebuffer geometry
  (including the hardware's *native* red/green/blue bit layout, so
  `PUREUNIX_VideoInit()` can pick the exact matching `SDL_PixelFormatEnum`
  and every blit is a plain per-row `memcpy`, no per-pixel repacking) and
  a whole-frame blit from a tightly-packed userspace buffer into real VRAM
  (`drivers/framebuffer.c`'s new `fb_blit_buffer()`), honoring the
  hardware's own pitch.
- **`SYS_SET_GRAPHICS_MODE`** — suspends/resumes `drivers/vga.c`'s console
  text repaint for the calling task's own VT (see "Graphics mode" below).
- **`SYS_GET_TICKS_MS`** — milliseconds since boot, backed by
  `arch/i386/pit.c`'s existing 100 Hz tick counter (so genuinely 10ms
  resolution despite the millisecond unit — finer than a 35 fps
  Doom-style tic, the workload this whole port exists for).
- **`SYS_FB_MMAP`** — see "Why a dedicated mapping, not a bigger heap"
  below.

None of these are in `user/libpure.h` (the raw, non-newlib syscall
surface) — SDL links against newlib only, like every other real port here
(ncurses, htop, ...); `user/pureunix_gfx.h` is the whole, deliberately
narrow interface between `src/video/pureunix/` and the kernel.

### Graphics mode: why the console needed a new flag

An SDL window and the ordinary text console share the same one physical
framebuffer and the same one VT. Before this port, `drivers/vga.c` only
ever asked "is this console the active one" before repainting it — never
"should this console's *pixels* be left alone even though it's active."
`console_t` gained a `graphics_mode` flag (`vga_console_set_graphics_mode()`)
and every existing hardware-paint gate (`force_draw()`, `cursor_draw()`,
`scroll()`, `console_reset()`'s pixel half, `vga_console_repaint()`, ...)
now checks a new `is_live(cs)` helper (`cs == g_active && !cs->graphics_mode`)
instead of a bare `cs == g_active` — deliberately *not* touching the
serial-mirror side effects (`serial_clear()`/`serial_putc()`), which never
touch the framebuffer and have no reason to care. `kernel/vt.c`'s
`vt_set_graphics_mode()` is the policy half: entering graphics mode is a
pure suppress; leaving it forces an immediate `vga_console_repaint()` if
the VT is still active, which is what makes "quit the SDL app, the shell
reappears exactly as it was" actually happen rather than leaving stale
framebuffer pixels on screen.

### Raw input: a second event source, not a replacement

`drivers/keyboard.c` (PS/2) and `drivers/hid.c` (USB Boot Protocol
keyboard) already fed a shared ASCII/`KEY_*` queue every tty/ncurses
program reads. Both now *also* call the new `vt_raw_input_push_key(code,
pressed)` for every press **and release** (the ASCII queue only reports
presses), using a shift-independent key identity (`'a'`, not `'A'`) so a
consumer can distinguish "which physical key" from "what character it
would produce" — the same distinction `SDL_Scancode` vs. `SDL_Keycode`
makes. `drivers/hid.c`'s USB keyboard driver needed a real diff-both-ways
pass (previous-report-has-it-current-doesn't) to synthesize releases at
all, since USB HID Boot Protocol reports only ever carry the currently-held
set, never an explicit release event.

### A new PS/2 mouse driver

`drivers/mouse.c` (new) — the legacy counterpart to a new
`drivers/hid.c` USB Boot Protocol mouse driver (`hid_mouse_try_attach()`,
mirroring the pre-existing keyboard driver pair exactly), both feeding the
same raw queue. Standard i8042 aux-port bring-up (disable both ports,
flush the output buffer, enable the aux port, set the controller command
byte's IRQ12-enable bit, `0xF6`/`0xF4` to the mouse device itself,
register IRQ12), decodes the standard 3-byte relative-motion packet
format. The kernel maintains one shared, clamped absolute pointer position
(`kernel/vt.c`) since mice report *relative* motion but SDL wants absolute
coordinates.

### Why a dedicated mapping (`SYS_FB_MMAP`), not a bigger heap

The first real QEMU boot test of this port failed immediately:
`SDL_GetWindowSurface failed: Out of memory`. QEMU's default granted
resolution is 1280x800x32bpp (~3.9 MiB) — bigger than this kernel's entire
3 MiB per-process address space window (`USER_WINDOW_BASE`/`END`,
`include/pureunix/vmm.h`) at the time, let alone the 1 MiB static heap
array every newlib program's `sbrk()` draws from
(`user/newlib_syscalls.c`'s `NEWLIB_HEAP_SIZE`). Simply enlarging that
shared heap constant would have been the wrong fix: `kernel/elf.c`'s
`PT_LOAD` loader eagerly allocates a *real physical frame* for every page
of a program's bss at `exec()` time (no demand paging in this kernel), so
growing a constant linked into *every* newlib program would cost `hello`,
`sqlite3`, `lua`, and everything else that many extra real MiB of RAM
whether they ever needed it or not.

Instead: the user window was widened from one 3 MiB PDE to four (16 MiB,
`USER_WINDOW_BASE`/`END`), and a brand-new fixed VA within it
(`FB_SHADOW_VA`, `include/pureunix/vmm.h`) — well clear of any real
program's code/data/heap/stack — is where `SYS_FB_MMAP` maps a process's
window-surface buffer *on demand*: real PMM frames are only ever allocated
for a process that actually calls it, via `vmm_map_page_in()`, the same
per-process-page-table primitive `fork()`'s address-space copy already
used. `kernel/vmm.c`'s `vmm_free_user_directory()` and
`vmm_fork_address_space()` — previously hardcoded to a single PDE index —
now loop over every PDE the (wider) window spans, so this new mapping is
freed/copied automatically by the exact same code path that already
handles a program's ordinary code/data/stack, with no separate cleanup
logic needed. `task_t.fb_shadow_mapped` makes a repeat `SYS_FB_MMAP` call
idempotent and is explicitly copied across `fork()` (`kernel/task.c`)
so a forked child's own later call reuses the already-copied mapping
instead of leaking it.

### A real, latent EXT2 multi-group bug this port's disk growth exposed

`sdltest.elf` (a real, statically-linked SDL2 app, even after
`-ffunction-sections`/`--gc-sections` dead-code stripping) no longer fit
in the old single-block-group 8 MiB EXT2 image
(`tools/mkext2.py`) alongside everything already installed.
`fs/ext2/alloc.c`'s block/inode allocator was already fully
group-generic — it iterates `fs->num_groups`, indexing `fs->bgdt[g]` —
so `tools/mkext2.py` was extended to build a real 3-block-group, 24 MiB
image (see that file's own "Why 3 block groups" comment), reusing group
0's exact byte-for-byte layout unchanged (so the regression suite's
fixed-offset fixtures, `bigfile.bin`/`hugefile.bin` in
`user/ext2test.c`/`user/systest.c`, keep passing unmodified) and adding
groups 1-2 as pure extra capacity.

Building this multi-group image the first time immediately hit a genuine,
previously-unexercised kernel bug: `fs/ext2/super.c`'s `num_groups`
computation (`ceil(s_blocks_count / s_blocks_per_group)`) doesn't subtract
`s_first_data_block` first the way the real EXT2 spec's formula does —
harmless at exactly one full group (where it happens to be correct by
coincidence), but with `FIRST_DATA_BLOCK=1` added into a 3-group total
block count, the kernel computed **4** groups, not 3 — a phantom group
whose BGDT entry `mkext2.py` never wrote (all-zero), which the allocator
then tried to use as real, corrupting allocation immediately (this is
exactly what produced the "Out of memory" error above, on top of the
address-space-size issue). Fixed on the generator side by making
`TOTAL_BLOCKS` an exact multiple of `BLOCKS_PER_GROUP` (not
`FIRST_DATA_BLOCK` + that), matching the kernel's own (technically
non-spec-compliant, but consistent) formula exactly rather than changing
the kernel's formula itself.

`user/systest.c`'s ENOSPC test (`MAX_FILLERS=128`, exactly sized to
overflow the *old* single-group 8 MiB image) silently stopped ever
reaching ENOSPC once the image grew to 24 MiB — not a crash, just a test
that stopped testing anything. Bumped to a generous, disk-size-independent
safety backstop (`MAX_FILLERS=4096`; the loop still exits the instant real
ENOSPC happens, so this costs nothing at runtime) so it can't silently
go stale again the next time the image grows.

---

## What's real, what's honestly not there yet

- **Video, software rendering, events (keyboard + mouse), timing** — all
  real, all covered above.
- **Audio** — cleanly unsupported (`SDL_AUDIO_DRIVER_DUMMY`): PureUNIX has
  no audio subsystem at all yet. `SDL_OpenAudioDevice()` succeeds and
  silently discards every buffer, exactly like the dummy driver does on
  any other platform, rather than failing outright — architected so a real
  driver can be added later (a new `src/audio/pureunix/` backend +
  clearing `SDL_AUDIO_DRIVER_DUMMY`) without touching anything above.
- **Threads** (`SDL_THREADS_DISABLED`) — PureUNIX's process model is
  fork()-style (separate address spaces); there is no shared-address-space
  lightweight thread primitive yet. `SDL_CreateThread()` fails cleanly
  (upstream's own generic thread backend's behavior when disabled) rather
  than silently doing nothing — this port's own video/event/timer loop
  never needs one.
- **Joystick/gamepad, haptic, sensor, HIDAPI** — no such input devices
  exist on this kernel yet; all cleanly disabled via `SDL_config_pureunix.h`
  (real upstream "not supported" behavior, not a stub crash).
- **Dynamic library loading** — no dynamic linker at all
  (`SDL_LOADSO_DISABLED`), same reasoning as `third_party/lua`'s
  `LUA_USE_DLOPEN` omission.
- **VT-switch-away-and-back while an SDL app is running** — implemented
  (`vt_set_graphics_mode()`'s suppression correctly survives a switch away,
  and `vga_bind_active()` naturally skips repainting a graphics-mode
  console when switched back to), but not exercised by this port's own
  testing — an SDL app doesn't currently re-blit on its own when it
  regains VT focus (no `SDL_WINDOWEVENT_EXPOSED` wiring yet), so the
  screen would show stale content until the app's own render loop next
  draws a frame. Real games/Doom-style loops redraw every frame regardless,
  so this is unlikely to matter in practice, but it's an honest gap, not a
  tested guarantee.

---

## Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `third_party/SDL2/SDL2-2.28.5/` | Vendored upstream source (pruned of `test/`, IDE/platform-specific project dirs — see that dir's `README.md`) | Not compiled by its own build system — see Architecture |
| `build/user/sdl2/libSDL2.a` | This port's curated object list, archived | Linked by any current or future SDL2 program, same shape as `NEWLIB_ELFS` |
| `/bin/sdltest.elf` | The real SDL2 test application (`user/sdltest.c`) | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF |

EXT2-only, like every other third-party userspace program in this tree —
FAT16 isn't part of the primary ash userland.

---

## Testing

Verified interactively in QEMU using the same QMP `send-key` +
`screendump` technique `tools/test-ncurses-demo.py`/htop's own testing
established, against **both** the ephemeral `build/pureunix-live.iso`
*and* the real, exact `make iso` persistent-disk deliverable
(`build/pureunix.iso`) — launching `/bin/sdltest.elf` from a genuine
BusyBox ash prompt on the actual artifact the task specifies, not just a
dev-loop build.

- **Real rendering**: `screendump` frames show the actual animated
  colored box on the real framebuffer content (not a placeholder/solid
  color), moving between frames — confirms the render loop, the window
  surface blit, and `SDL_Delay()`-paced timing are all genuinely live.
- **Keyboard input**: arrow-key presses via QMP `send-key` visibly change
  the box's velocity between frames.
- **Mouse input**: injected relative motion (QMP `input-send-event`,
  `type: rel`) plus a button click visibly relocates the box to the
  click-time cursor position, confirmed by `drivers/mouse.c`'s own
  `mouse: PS/2 mouse attached (IRQ12)` boot log line and a large,
  bounce-physics-inconsistent jump in the box's position between
  screenshots.
- **Clean exit**: Escape quits the app; the very next screendump shows the
  ordinary text console fully restored (no leftover framebuffer garbage);
  the shell prompt reappears and correctly executes a subsequent command,
  confirming canonical/echo tty state was never left disturbed.
- **Real persistence**: `tools/test-persistent-boot.py` against a copy of
  the real `make iso` output still passes unchanged (two independent QEMU
  boots, second one reading back a file the first one wrote) — confirms
  the widened EXT2 image and enlarged per-process address space didn't
  regress the persistent-disk story this kernel's other major milestone
  established.
- **Full regression suite unchanged**: `tools/vt-scripts/*.txt` (ash job
  control, Ctrl+C/Ctrl+\ foreground signal delivery, `ps`/`top` via
  `/proc`, VT-switch-doesn't-interrupt) all pass; `systest` 343/345 (same
  2 pre-existing console-geometry failures, `[129]`/`[130]` — see project
  memory's `gotcha_preexisting_systest_failures`; unrelated to this port).

---

## Known gaps

- No audio, threads, joystick/gamepad/haptic/sensor, or dynamic loading —
  see "What's real, what's honestly not there yet" above for the reasoning
  behind each and how a real implementation could be added later without
  disturbing this port's architecture.
- No `SDL_WINDOWEVENT_EXPOSED` on regaining VT focus after an Alt+F<n>
  switch away and back — see the "VT-switch-away-and-back" note above.
- `SYS_FB_BLIT` only supports a full-frame blit, not partial/dirty-rect
  updates — a real performance optimization opportunity, not a
  correctness gap (`SDL_UpdateWindowSurface()`'s callers already expect
  the whole current window content to end up visible either way).
- Only tested with one SDL window per process, and only one SDL
  application running at a time — matches the single-active-VT-owns-the-
  screen model this whole graphics-mode design assumes; multiple
  concurrent SDL apps on different VTs was not tested.
