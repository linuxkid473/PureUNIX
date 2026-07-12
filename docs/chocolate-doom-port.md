# Chocolate Doom Port

## Overview

Real, unmodified-core [Chocolate Doom](https://www.chocolate-doom.org) 3.1.1
runs natively on PureUNIX as `/bin/chocolate-doom` (also reachable as the
plain PATH command `chocolate-doom`, no `.elf` needed): a genuinely
playable Doom session — real title screen, real menu, real gameplay
rendering, real keyboard/mouse input, real timing — through the SDL2 port
(`docs/sdl-port.md`) this was built to prove out. This is the "larger
purpose" that whole port existed for.

```
# chocolate-doom -iwad /bin/doom1.wad
    (real title screen -> menu -> E1M1 gameplay, rendered through
     PureUNIX's native framebuffer, reacting to real keyboard/mouse input)
# (in-game menu "Quit Game" -> y -- shell prompt reappears exactly as it was)
```

The IWAD ships in the default image at `/bin/doom1.wad` (see Install
layout) — no manual placement needed on a freshly booted `make iso`
image. Ctrl+S also works as an unconditional kill switch any time an SDL
app (Chocolate Doom or `sdltest`) owns the screen — see "Graphics-mode
input hardening" below.

Verified against the real, exact `make iso` persistent-disk deliverable —
including a genuine two-boot persistence test (fresh QEMU process, same
disk image file) proving both the IWAD and Chocolate Doom's own config
files survive a reboot. See Testing.

---

## Architecture

### Real upstream source, zero patches

`third_party/chocolate-doom/chocolate-doom-3.1.1/` is an unmodified
extraction of the upstream 3.1.1 release source (see that directory's
`README.md` for the vendoring rationale and exactly why no upstream file
needed changing — every PureUNIX-specific decision turned out to already
be a real, upstream-supported configuration, expressed entirely through a
hand-written `include/config.h` standing in for what Chocolate Doom's own
CMake `configure_file()` step would otherwise generate, the same role
`SDL_config_pureunix.h` plays for the SDL2 port). Only the `chocolate-doom`
binary itself is built — not `chocolate-server`, `chocolate-setup`, or the
Heretic/Hexen/Strife binaries the same source tree also produces — since
none of those are needed to make real Doom gameplay work, and setting up
key bindings/video options through `chocolate-setup`'s interactive GUI
isn't required either (Chocolate Doom's own config file defaults and
command-line arguments are sufficient — see `-iwad`/`-warp`/`-skill` in
Testing below).

PureUNIX's top-level `Makefile` compiles a curated file list — copied
directly from upstream's own `src/CMakeLists.txt` and `src/doom/
CMakeLists.txt` — the same "hand-list the real files, don't hand-run the
real build system" pattern every `third_party/` port here uses, so this
stays a faithful reproduction of the actual upstream link graph rather
than a guess.

### Why it just worked: the SDL2 port had already done the hard part

The single biggest fact about this port: **the entire Chocolate Doom
codebase compiled and linked against PureUNIX's newlib + SDL2 port with
zero source changes on the very first full build attempt.** Every
platform capability real Doom gameplay needs — real file I/O (`fopen`/
`fread`, not `mmap` — see below), real `getenv`/`mkdir` for its config/save
directories, real `strcasecmp`, a real SDL2 window/renderer/event/timer
stack — had already been proven out by the SDL2 port and its own
predecessors (ncurses, SQLite, htop, ...). This port's real work turned
out to be two *general* PureUNIX platform fixes (below), not anything
Doom-specific.

### Two real, upstream-supported configurations, not hacks

- **`DISABLE_SDL2MIXER=1` / `DISABLE_SDL2NET=1`** (`include/config.h`) —
  real, documented upstream CMake options
  (`-DENABLE_SDL2_MIXER=OFF -DENABLE_SDL2_NET=OFF`), not a PureUNIX
  invention. PureUNIX has no `SDL2_mixer`/`SDL2_net` vendored (no dynamic
  linker to load them against regardless — see `third_party/SDL2/
  README.md`'s `SDL_LOADSO_DISABLED` reasoning), so the build takes the
  exact same real, complete `#else` branch a desktop build without those
  libraries installed would, in `i_sdlmusic.c`/`i_sdlsound.c`/
  `i_musicpack.c`/`net_sdl.c`.
- **No `HAVE_MMAP`** — `w_file.c`'s `W_OpenFile()` only ever tries the
  `mmap()`-backed WAD reader if the user explicitly passes `-mmap`;
  the default (and this port's only) path is the plain `fopen()`/
  `fread()`-based reader, real and always-compiled upstream, not a
  fallback invented here. PureUNIX's own `mmap()` only supports anonymous
  scratch allocations anyway (`user/newlib_compat/sys/mman.h`), so this
  was the only real option regardless — and it was already upstream's own
  default.
- **The always-present `opl`/`pcsound` fallback audio modules still link
  and initialize** even with real audio unavailable: `S_Init` calls real
  `SDL_OpenAudioDevice()`, which succeeds against the SDL2 port's dummy
  audio driver exactly like it would on any platform with no real audio
  hardware. `OPL_Init: Failed to find a working driver. Dude. The Adlib
  isn't responding.` is Chocolate Doom's own, real, upstream message for
  "no OPL synth chip present" — the same message a real DOS machine
  without a Sound Blaster would print — not a PureUNIX-specific failure.

### Two real PureUNIX platform bugs this port found and fixed

Found the same way every other port here found its platform gaps: by
actually building, booting, and running the real thing, not by
inspecting the source in the abstract.

**1. `sbrk()` used to be a fixed-size static array — too small for Doom's
zone allocator, and the wrong design regardless.**

Chocolate Doom's memory manager (`src/z_zone.c`) mallocs its entire
private zone heap in one shot at startup — 16 MiB by default
(`I_ZoneBase()`/`AutoAllocMemory()` in `src/i_system.c`), falling back
only as low as 4 MiB if that fails. PureUNIX's `sbrk()`
(`user/newlib_syscalls.c`) used to be a plain pointer bump through a
fixed-size static array in every newlib program's own bss — sized at only
2 MiB (previously bumped once already for the SDL2 port, see
`docs/sdl-port.md`'s own "eager bss physical allocation" note) — nowhere
near enough, and bumping that shared array to fit Doom would have meant
every other newlib program (`hello`, `sqlite3`, `lua`, ...) paying that
many extra real physical MiB at `exec()` time too, whether they used that
heap or not (`kernel/elf.c`'s `PT_LOAD` loader has no demand paging — it
allocates a real frame for every page of a program's bss up front).

**Fix**: a real, incrementally-grown `sbrk()`, backed by a new syscall
(`SYS_SBRK`, `arch/i386/syscall.c`). The kernel maps one real physical
page at a time as a task's heap break actually grows past
`task_t.heap_mapped`, up to `HEAP_MAX` (32 MiB, `include/pureunix/vmm.h`'s
`HEAP_VA`/`HEAP_MAX`) — replacing the static array entirely. This is a
strict improvement for *every* newlib program, not just Doom: a tiny
program now costs only the handful of pages it actually touches (down
from a flat 2 MiB every program paid before, whether it needed it or
not), while a program that genuinely needs tens of MiB — like Chocolate
Doom's zone allocator — can still get them. `task_t.heap_used`/
`heap_mapped` are explicitly copied across `fork()` (`kernel/task.c`),
alongside the VA mapping itself, which `vmm_fork_address_space()`'s
existing whole-window deep copy already handles.

**2. The per-process address space window had to grow again, this time
to 64 MiB.**

The SDL2 port had already widened the window once, from 3 MiB to 16 MiB,
specifically to fit its own on-demand framebuffer mapping
(`FB_SHADOW_VA`). Chocolate Doom needed both that *and* up to 32 MiB of
real incremental heap simultaneously (it uses SDL2 for its own video, on
top of its zone allocator) — more than 16 MiB has room for. Widened to 64
MiB (`USER_WINDOW_BASE`/`END`, `include/pureunix/vmm.h`), with `HEAP_VA`
placed at a fixed offset comfortably clear of any real program's ELF
image, and `FB_SHADOW_VA` placed right after `HEAP_VA`'s own maximum
extent so the two on-demand regions can never collide. This costs
nothing by itself — `vmm_map_page_in()` only ever spends a real physical
frame for a page some process actually touches, whether via `HEAP_VA`'s
incremental growth or `FB_SHADOW_VA`'s one-shot mapping — unlike the
eager-bss-array design these on-demand regions replaced.

Both fixes were verified with the full existing regression suite
(`systest` 343/345, same 2 pre-existing failures; all `tools/vt-scripts/
*.txt`) re-run and passing unchanged after each change, given how many
other programs' memory model this touches.

### Graphics-mode input hardening: two more real bugs found via live testing

Playing the game for real (not just scripted QEMU tests) surfaced two
more general platform bugs, both in how keyboard input interacts with
`graphics_mode` (`drivers/vga.c`'s existing output-suppression flag,
introduced by the SDL2 port) — neither specific to Chocolate Doom, both
equally applicable to `sdltest`.

**1. Keystrokes typed during a graphics-mode session leaked into the
underlying shell's own input queue.** `drivers/keyboard.c`'s IRQ handler
always fed every ordinary key into *both* the raw input queue an SDL app
polls (`vt_raw_input_push_key()`) *and* the ASCII canonical-mode queue
`ash`'s `tty_read()` eventually consumes (`vt_input_push()`), regardless
of whether the active VT was in graphics mode. Since an SDL app never
drains that second queue, every key pressed during gameplay (menu
shortcuts, movement keys, ...) sat there unread — then replayed all at
once as garbage input the moment the app exited and the shell resumed
reading, corrupting whatever command the user typed next (observed
directly: quitting Chocolate Doom via Escape→q→Enter→y left `^[q` echoed
as an unknown command, with the trailing `y` silently prepended to the
next line typed). **Fix**: `vt_input_push()` (`kernel/vt.c`) now drops
ordinary keys entirely while its VT is in graphics mode — mirroring the
exact gate `drivers/vga.c` already applies to output — leaving Ctrl+C/Z/\
signal delivery (which happens earlier in the same function) untouched.

**2. A hard-killed graphics app left the console permanently stuck.**
Reported directly during testing: pressing Ctrl+C while `sdltest` was
running "froze" that VT. Root cause: `kernel/signal.c`'s default-action
paths for SIGKILL and every other default-terminate signal (SIGINT
included) set `target->state = TASK_ZOMBIE` directly, bypassing
`task_exit()` entirely — so a killed SDL app never got a chance to call
`SDL_DestroyWindow()`/`SYS_SET_GRAPHICS_MODE(0)`. Combined with fix #1
above, a VT stuck in `graphics_mode` after such a kill would silently
drop every future keystroke forever, with no way back short of a reboot.
**Fix**: both `signal_send()`'s hard-termination paths and `task_exit()`
now force `graphics_mode` off (and repaint) for the dying task's own VT
if it still had it enabled — belt-and-suspenders coverage for every way
a task can stop running, not just a clean SDL exit. Verified: Ctrl+C
during `sdltest` now recovers correctly (shell prompt back, next typed
command runs), full regression suite unchanged.

**New feature, built on fix #2's cleanup path**: Ctrl+S is now an
unconditional kill switch whenever the active VT is in graphics mode —
sends real, unblockable `SIGKILL` to the whole foreground process group
(`kernel/vt.c`'s `vt_input_push()`), guaranteed to return the VT straight
back to its `ash` shell in one keypress regardless of whether the app is
hung, ignoring signals, or otherwise unresponsive. Deliberately scoped to
graphics mode only, so the pre-existing `KEY_CTRL_S` = "save" binding in
the legacy in-kernel editor (`editor/editor.c`) is completely unaffected
outside it. Verified against both `sdltest` and a genuinely mid-gameplay
Chocolate Doom session (screendump taken immediately before the
keypress): `^S: killing graphics app` / `Killed` echo, shell fully
responsive to a subsequent typed command afterward.

### Arrow keys were silently shifted one constant off (fixed)

Real play-testing surfaced a third, more subtle bug, reported directly:
"the arrow keys are like wrong ... shooting works and all the others."
Empirically, holding Up/Left/Down all produced correct movement/turning,
but Right produced *no visible effect at all* — not inverted, just inert.

Root cause: `include/pureunix/keyboard.h`'s `KEY_BASE = 0x100` is itself
a real enum member, so the very next enumerator (`KEY_UP`, with no
explicit value) silently became `KEY_BASE + 1` (0x101), not 0x100 — and
every constant after it inherited the same +1 drift (`KEY_RIGHT` = 0x104,
not 0x103; `KEY_DELETE` = 0x109, not 0x108; and so on). Meanwhile
`user/pureunix_gfx.h`'s independently-numbered `PU_KEY_*` constants (used
by `SDL_puevents.c` to translate a raw input event into an SDL scancode)
were numbered correctly, since that header explicitly writes `PU_KEY_UP =
PU_KEY_BASE` — no implicit gap. The two enums silently disagreed by
exactly one slot: the kernel's `KEY_RIGHT` (0x104) numerically collided
with userspace's `PU_KEY_HOME` (0x104), so pressing the physical Right
arrow was delivered to Chocolate Doom as `SDL_SCANCODE_HOME` — unbound,
hence "no effect". Confirmed directly via a temporary raw-scancode debug
print (`drivers/keyboard.c`'s `keyboard_irq()`) before fixing: every one
of Up/Down/Left/Right/Home/End/PageUp/PageDown/Delete came out exactly
one higher than its `PU_KEY_*` counterpart expected.

This bug was invisible to every *other* consumer of arrow keys
(ncurses/`vi`/`htop`'s Up/Down/Left/Right all worked fine throughout)
because `drivers/tty.c`'s `key_to_escape_seq()` compares against these
same kernel-side symbolic constants directly and never crosses into
`PU_KEY_*` numbering — only the raw SDL input path (Chocolate Doom,
`sdltest`) is affected, which is exactly why this went unnoticed until
now. **Fix**: `KEY_UP = KEY_BASE` made explicit (matching the pattern
`user/pureunix_gfx.h` already used), removing the implicit gap. Verified
via the same debug print (now removed) showing corrected values, then
empirically via pixel-diff magnitude (Right went from ~3,000 differing
bytes out of 3M — noise-level — to ~1.1M, matching Left's magnitude) and
visual confirmation that the rotation direction is correct. Full
regression suite unchanged.

### A cosmetic, harmless renderer warning

`CreateUpscaledTexture: Limited texture size to 0x0 (max 16000000 pixels,
max texture size 0x0)` appears in the log on every launch — the SDL2
port's software renderer reports a maximum texture size of 0 (no texture
size limit is meaningful for a pure software rasterizer, unlike a real
GPU), which Chocolate Doom's own `CreateUpscaledTexture()` logs a warning
about but handles gracefully regardless — rendering, menu, and gameplay
are all unaffected, confirmed by every screenshot in Testing below.

---

## Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `third_party/chocolate-doom/chocolate-doom-3.1.1/` | Vendored upstream source (pruned of `.devcontainer/`, `.github/`, `win32/` — see that dir's `README.md`) | Not compiled by its own build system — see Architecture |
| `third_party/chocolate-doom/config/config.h` | Hand-written platform config | Stands in for CMake's `configure_file()` step |
| `/bin/chocolate-doom.elf` | The real engine binary | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF |
| `/bin/chocolate-doom` → `/bin/chocolate-doom.elf` | Symlink | Same pattern as `htop`/`ncdemo`/`sqlite3` — makes `chocolate-doom` resolve as an ordinary PATH command with no setup |
| `third_party/chocolate-doom/doom1.wad` (host source) → `/bin/doom1.wad` (image) | The real, freely-distributable id Software shareware IWAD (Doom v1.9, sha1 `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`) | `Makefile`'s `CHOCDOOM_IWAD` variable, placed via `mkext2.py`'s `--extra-file` flag — ships by default so `chocolate-doom -iwad /bin/doom1.wad` works immediately after a fresh boot, zero manual setup |

EXT2-only, like every other third-party userspace program in this tree —
FAT16 isn't part of the primary ash userland. Chocolate Doom's own config
files (`default.cfg`, `chocolate-doom.cfg`) are **not** part of the image
build — Chocolate Doom creates them itself on first run, in whatever
directory it's launched from (`/root/` is a natural, already-writable
choice — see Testing's persistence check). A user's own retail IWAD can
be placed anywhere on the persistent filesystem the same way and passed
via `-iwad`; it doesn't need to replace or coexist with `/bin/doom1.wad`.

`tools/mkext2.py` gained a small, generically useful `--extra-file
HOST:DEST` option (places one arbitrary host file at an arbitrary
absolute path on the image) — now used permanently by the `Makefile` to
place `doom1.wad` at `/bin/doom1.wad` in every build, and reusable for
placing any other file at an arbitrary path in future ports/testing.
Finding a real bug while adding it: `ensure_dir()`'s cache was
never consulted against directories `main()` itself creates directly
(`/root`, `/home`, ...), so placing a file under `/root` silently created
a second, shadowing, unreachable `root` directory entry — fixed by having
`ensure_dir()` also check the real directory-entry list of an
already-built parent, not just its own creation cache, before creating a
new one (a real correctness fix to a pre-existing helper, not new
special-casing for this port).

---

## Testing

Verified interactively in QEMU using the same QMP `send-key` +
`screendump` technique every other port's testing here uses, against the
real, exact `make iso` persistent-disk deliverable — not a synthetic or
compile-only check. The IWAD used was `doom1.wad` (id Software's Doom
shareware episode, v1.9, sha1 `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`)
— explicitly, permanently freely distributable by id Software's own
longstanding policy, obtained from the Internet Archive's `doom-wad-
shareware_1.9` mirror (sourced from the Debian `doom-wad-shareware`
package, itself sourced from doomwiki.org's own "How to download and run
Doom" page) — a real, legally-obtained IWAD, not a placeholder. It now
ships at `/bin/doom1.wad` in every default build (see Install layout),
so the completion check below was run against a freshly booted, entirely
unmodified `build/pureunix.iso` — booted straight to `/`, no `cd`, no
`--extra-file` test scaffolding, no PATH setup: `ls -la /bin/doom1.wad`
confirmed present at its original 4196020-byte size, then `chocolate-doom
-iwad /bin/doom1.wad -warp 1 1` reached real gameplay directly.

- **Real title screen**: `chocolate-doom -iwad doom1.wad` renders the
  actual `DOOM` shareware title screen (id Software logo, marine artwork,
  "PROVIDED BY ID FREE OF CHARGE" banner) — genuine upstream graphics
  decoded from the real IWAD's lumps, not a placeholder.
- **Real menu**: Escape from gameplay shows the actual in-game pause menu
  (New Game/Options/Load Game/Save Game/Read This!/Quit Game) overlaid on
  the paused game view behind it; keyboard shortcuts (first-letter
  selection, e.g. `q` for Quit Game) and cursor navigation both move the
  skull selector correctly.
- **Real gameplay**: `-warp 1 1` (or via the menu) reaches actual E1M1
  ("Hangar") gameplay — real 3D-perspective software-rendered geometry
  (walls, the map's blue nukage pool, pillars), the real HUD (ammo/
  health/armor/face/arms/ammo-type readout), all genuinely decoded from
  the IWAD, not a mock.
- **Real input**: injected keyboard events (arrow keys) visibly change
  the player's view angle/position between screenshots during gameplay;
  menu navigation and shortcut keys are correctly recognized.
- **Real timing**: `Z_Init`/`R_Init`/etc. all report real init progress;
  the title screen renders steadily across multiple screendumps with no
  tearing or corruption, consistent with the SDL2 port's own
  `SYS_GET_TICKS_MS`-backed timing (`docs/sdl-port.md`) driving a real
  35 tics/sec game loop.
- **Clean exit**: `Quit Game` → `y` genuinely terminates the process (not
  force-killed by the test harness) — confirmed by the shell prompt
  reappearing and correctly executing and reporting errors for
  subsequent typed commands (proving canonical-mode/echo tty state was
  never left disturbed), with no leftover framebuffer graphics on screen.
- **Real persistence, across an actual reboot**: a two-boot test (fresh
  QEMU process each time, same disk image file — the same technique
  `tools/test-persistent-boot.py` already established) — boot 1 places
  `doom1.wad` via `--extra-file`, launches and quits Chocolate Doom
  (creating `default.cfg`/`chocolate-doom.cfg` on first run); boot 2,
  against the same on-disk image, confirms `ls -la /root` shows
  `doom1.wad` (exact original byte size, 4196020 bytes), `default.cfg`,
  and `chocolate-doom.cfg` all still present.
- **Full regression suite unchanged**: `tools/vt-scripts/*.txt` (ash job
  control, Ctrl+C/Ctrl+\ foreground signal delivery, `ps`/`top` via
  `/proc`, VT-switch-doesn't-interrupt) all pass; `systest` 343/345 (same
  2 pre-existing console-geometry failures, `[129]`/`[130]`) — re-verified
  after *each* of the two memory-model changes above, given how many
  other programs' address space they touch.

---

## Known gaps

- **No real audio** — inherited directly from the SDL2 port's own scope
  (`docs/sdl-port.md`); Chocolate Doom's `opl`/`pcsound` fallback modules
  initialize successfully against the dummy SDL audio driver and produce
  no audible sound, exactly like a real machine with no sound hardware.
- **No multiplayer** — `SDL2_net` is disabled (see Architecture); single-
  player is fully unaffected (`net_loop.c`'s real loopback "solo game"
  path handles it, the same one a real single-player session on any
  platform uses).
- **No `chocolate-setup` GUI** — not built (see Architecture); every
  option it would configure is reachable via Chocolate Doom's own
  command-line arguments or its plain-text config file instead.
- **`SDL_GetPrefPath` fails** (`M_SetMusicPackDir: SDL_GetPrefPath
  failed, music pack directory not set`) — PureUNIX's SDL2 port doesn't
  implement it (no real per-user "preferences directory" concept exists
  yet); Chocolate Doom handles the failure gracefully and simply doesn't
  set a music pack directory, which is irrelevant with audio unavailable
  regardless.
- **Only tested with the Doom shareware IWAD** — Heretic/Hexen/Strife
  binaries aren't built (see Architecture); a full retail Doom/Doom II
  IWAD should work identically (same `w_wad.c`/engine code path) but
  wasn't tested here, deliberately, to keep this port's own testing using
  only content id Software has always freely distributed.
