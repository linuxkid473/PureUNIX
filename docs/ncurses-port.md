# ncurses Port

## Overview

[ncurses](https://invisible-island.net/ncurses/) 6.5 runs natively inside
PureUNIX — real, unmodified upstream ncurses (`libncurses.a`/`libpanel.a`/
`libmenu.a`/`libform.a`), cross-compiled for `i686-elf` and linked into real
ring-3 PureUNIX ELF processes, not a fake curses clone or a host-side
emulation. `/bin/ncdemo` is a genuine interactive full-screen TUI
application built against it:

```
# ncdemo
+- ncdemo --------------------------------------------------------------+
| Colors demo                                                           |
| Cursor keys demo                                                      |
| Reverse/bold attribute demo                                           |
| Quit                                                                  |
|                                                                        |
| Up/Down + Enter, q to quit                                            |
| 47x160                                                                |
+------------------------------------------------------------------------+
```

ncurses is vendored under `third_party/ncurses/` (see that directory's
`README.md`) and documented here for the parts specific to *this* port:
why its build works differently from TCC/Lua/SQLite's, what had to be
added at the kernel/driver/libc layers for a real terminal to exist under
it, two genuine bugs this port found (one in this port's own terminfo
source, one a cross-compilation blind spot in ncurses' own `configure`),
the install layout, and testing.

---

## Architecture

### Why ncurses' own `configure`/`make`, unlike TCC/Lua/SQLite

Every other vendored third-party dependency in this tree (`third_party/
tcc/`, `third_party/lua/`, `third_party/sqlite/`) is built by PureUNIX's
own top-level Makefile compiling a flat, hand-enumerated list of upstream
`.c` files — "vendor upstream source, compile it with our own Makefile
rules, no upstream build system." That works because those projects' own
builds are simple enough to reproduce faithfully by hand.

ncurses is not: a substantial fraction of what actually gets compiled is
generated *at build time* by ncurses' own tooling — `curses.h`/`term.h`
from templates substituted with configure-derived values, capability
tables generated from its `Caps` database via `awk` scripts, and (via
`--with-fallbacks`) a compiled-in terminfo table produced by running
ncurses' own `tic` over a terminfo source file. Hand-reproducing that with
PureUNIX-authored Makefile rules would mean re-deriving hundreds of
generated lines by hand instead of vendoring them — not a faithful
reproduction the way Lua's object list is.

This is exactly the same situation `third_party/newlib/` is already in
(also a real, generated, configure-driven build), and this port follows
that same precedent: `tools/build-ncurses.sh` — mirroring `tools/
build-newlib.sh` — runs ncurses' real `configure`/`make` **once**, cross-
compiling with the same `i686-elf-gcc` and newlib flags every other
PureUNIX userspace binary uses, and vendors the *build output* (headers +
static libs) into `third_party/ncurses/i686-elf/`, committed. `make`/
`make iso` themselves never invoke autotools and stay network-free —
they just link against `third_party/ncurses/i686-elf/lib/libncurses.a`
the same way they already link against `third_party/newlib/i686-elf/lib/
libc.a`, via a plain `-isystem`/`-L` pair (`NCURSES_CFLAGS`/
`NCURSES_LDFLAGS` in the Makefile's ncurses section).

### Cross-compiling ncurses itself

`tools/build-ncurses.sh` configures ncurses with `--host=i686-elf
--with-build-cc=cc` — ncurses' own documented cross-compilation support:
a handful of internal code-generator programs (`make_hash`, `make_keys`,
and `tic`/`infocmp` for the fallback-table step below) have to run on the
*build* machine, not the target, since their output is needed *during*
the build, not after it. `--with-build-cc` tells ncurses' configure to
compile those specific programs with the host compiler while everything
else uses the real `i686-elf-gcc` cross compiler.

A second, narrower host build (also inside `build-ncurses.sh`) produces a
`tic`/`infocmp` built from the *exact same* vendored source tree, passed
via `--with-tic-path`/`--with-infocmp-path` — needed because the system
`/usr/bin/tic` on a macOS host is a much older ncurses release and
chokes partway through compiling the (large, ~4000-entry) terminfo
database `--with-fallbacks` needs to scan.

`--disable-database --with-fallbacks=pureunix,dumb`: PureUNIX has no
terminfo database on its filesystem at all, so every `TERM` lookup has to
resolve through a table compiled directly into `libncurses.a`. Only two
entries are compiled in — this port's own `pureunix` (below) and the
standard `dumb` as a documented, harmless fallback (never actually
reached — every VT sets `TERM=pureunix`, `kernel/main.c`).

`--without-shared --without-cxx --without-cxx-binding --without-ada
--without-manpages --without-tests --without-progs`: no dynamic linker
exists at all (same reasoning as SQLite's `SQLITE_OMIT_LOAD_EXTENSION`),
and this port's scope is the library + a real interactive demo
application, not ncurses' own C++ bindings, test suite, or `tic`/
`infocmp`/`tput` CLI tools on the target. `--disable-widec`: an 8-bit,
non-wide-character build — `drivers/font.c`'s bitmap font has no Unicode
glyph coverage, so there is nothing for wide-character support to render
against yet.

### The "pureunix" terminfo entry

`third_party/ncurses/pureunix.terminfo` is PureUNIX's own terminfo
*source* description — a new terminal type, not a patch to any ncurses
file (adding a new terminal is always done this way: ncurses' own `misc/
terminfo.src` is nothing but ~4000 entries in exactly this format).
`build-ncurses.sh` appends it to a scratch copy of that file before
configuring (the pristine vendored `ncurses-6.5/` source tree is never
modified in place).

Every capability in it traces to a specific, real escape sequence
`drivers/vga.c`'s parser actually implements or `drivers/tty.c` actually
emits for a keypress — colors (`setaf`/`setab`, the 16-color ANSI
palette), cursor motion (`cup`/`cuu`/`cud`/`cuf`/`cub` and their `1`
single-step forms), cursor visibility (`civis`/`cnorm`, DECTCEM),
save/restore cursor (`sc`/`rc`, DECSC/DECRC), scrolling (`ind`/`ri`,
index/reverse-index; `csr`, DECSTBM scroll regions), line insert/delete
(`il`/`dl`), erase (`ed`/`el`/`el1`), reverse video (`rev`/`smso`/`rmso`),
and the full set of special-key sequences (`kcuu1`/`kcud1`/`kcuf1`/
`kcub1`, `khome`/`kend`/`kpp`/`knp`/`kdch1`, `kf1`..`kf12`). A capability
this port doesn't implement (mouse reporting, an alternate screen buffer,
underline/blink/dim rendering, hardware tab stops) is simply **omitted**,
not declared — ncurses degrades gracefully for a real absent capability
(e.g. no `dch1`/`ich1` just means a full line rewrite instead of a
single-character shift), which is the correct, standard way to describe
a terminal that genuinely lacks a feature.

---

## Platform work this needed

Everything below was found by actually compiling and running `ncdemo`
inside PureUNIX under QEMU and iterating — including two genuine bugs
only found by driving real keystrokes through a real running program, not
by reading source or reasoning about the terminal model in the abstract.

### 1. `drivers/vga.c`'s ANSI/VT100 parser was missing most of what a real
full-screen application needs

Before this port, the console driver implemented enough ANSI/VT100 to
render colored shell/editor output (SGR 16-color, `ED`/`EL`, absolute
cursor positioning, a DECSTBM scroll region, `IL`/`DL`) but nothing a
`initscr()`/`refresh()`-driven application actually depends on:

- **DECTCEM cursor visibility** (`ESC[?25l`/`ESC[?25h`) — added, including
  a real hardware-cursor-disable path for the legacy VGA text-mode
  fallback (`set_text_cursor_visible()`), not just the framebuffer path.
- **Relative cursor motion** (`CUU`/`CUD`/`CUF`/`CUB`, `ESC[nA`/`B`/`C`/`D`)
  — added; the parser previously only understood absolute positioning.
- **DECSC/DECRC save/restore cursor** (bare `ESC 7`/`ESC 8`, not
  CSI-prefixed) and **IND/RI index/reverse-index** (bare `ESC D`/`ESC M`)
  — added a second, non-CSI branch to the escape-state machine for these
  (`vga_putc_vt()`); `RI` at the scroll region's top margin reuses
  `insert_lines(cs, 1)` unchanged (identical effect), and `IND` at the
  bottom margin reuses the same `scroll()` `newline()` already called.
- **Reverse video and bold** (`SGR 7`/`27`, `SGR 1`/`22`) — deliberately
  *not* implemented as new per-cell storage (doubling every console's
  memory footprint for one bit of state). Reverse is a physical fg/bg
  nibble swap that's its own inverse (`set_reverse()`); bold brightens a
  *subsequently*-set base color into its bright variant
  (`apply_fg()`) — the same trick the existing 16/90-97 "bright" ANSI
  codes already used, just generalized. `SGR 39`/`49` (default fg/bg) got
  the same treatment.
- **BEL** (`\007`) previously fell through to being rendered as a garbage
  glyph; now a real (silent — no bell hardware) no-op.

None of this touched the existing SGR color/`ED`/`EL`/`CUP`/`CHA`/
DECSTBM/`IL`/`DL` handling, and the escape buffer (`esc_buf[16]` →
`esc_buf[32]`) was widened only for headroom on longer parameterized
sequences (e.g. `DECSTBM`'s `24;1r`), not because any of the above needed
it.

### 2. Arrow/function keys were silently dropped at the keyboard→tty layer

`drivers/keyboard.c` already decoded arrow keys, Home/End/PgUp/PgDn/
Delete into sentinel `KEY_*` ints (used by the in-kernel editor and
BusyBox's own line editor) — but `drivers/tty.c`'s `key_to_byte()`
(the function that turns a decoded keypress into the byte(s) a `read()`
caller actually sees) mapped every one of them to `-1` and silently
dropped it. No ncurses application can use its own arrow-key navigation
without them.

**Fix**: `key_to_escape_seq()` (`drivers/tty.c`) translates each special
key into the exact multi-byte ANSI/xterm sequence the `pureunix`
terminfo entry declares for it (`KEY_UP` → `"\033[A"`, `KEY_F5` →
`"\033[15~"`, ...), queued in a small per-VT pending-byte buffer
(`pending_buf[NUM_VTS]`) that both the canonical and raw-mode read paths
drain before pulling a new keypress — so a 3-5 byte escape sequence from
one keypress correctly survives being split across several separate
`read()` syscalls (which is exactly how ncurses itself reads input, one
byte per `read()` call). `drivers/keyboard.c` also gained F1..F12
decoding (Set-1 scancodes 0x3B-0x44, 0x57-0x58; bare, since Alt+F1-6 is
already claimed for VT switching).

A nice side effect: BusyBox ash's own line editor already looks for
`ESC[A`/`ESC[B` to implement command-history recall — previously
unreachable dead code, since arrows never reached `read()` at all. Up/Down
history navigation at the shell prompt now works too, not just in
ncurses applications.

### 3. SIGWINCH didn't exist

`include/pureunix/signal.h` had no `SIGWINCH` at all. Added (`28`,
matching newlib's BSD numbering for this target — see project memory's
`gotcha_bsd_signal_numbering`), plus `vt_signal_resize()`
(`kernel/vt.c`), broadcasting it to every VT's foreground process group
whenever the console geometry actually changes — currently only the
`font`/`TIOCSFONT` scale-change path (`arch/i386/syscall.c`), the one
existing way this kernel's console geometry can change at runtime. `/bin/
ncdemo` installs a real `SIGWINCH` handler and calls `resizeterm()`/
`wresize()` from it as a demonstration, though real dynamic mid-session
resizing is inherently a narrow case for a framebuffer console with no
window manager.

### 4. `TERM` was never set — every ncurses program failed outright

The very first end-to-end test failed immediately: `initscr()` returned
`"Error opening terminal: unknown."`. `kernel/main.c` seeds a handful of
default environment variables (`USER`/`HOME`/`SHELL`) once at boot,
inherited by every VT session's shell — `TERM=pureunix` needed adding
right alongside them (one shared global env table, `shell/builtins.c`,
so setting it once covers every VT the same way the others already do).

### 5. A real, reproducible crash traced to this port's own terminfo source

**Symptom**: any ncurses program crashed the whole kernel (`KERNEL PANIC:
CPU exception 14 (page fault)`) the instant `initscr()` was called — not
a hang, not a graceful failure, a full page fault with `cr2=0xffffffff`.

**Root cause**: an earlier draft of `pureunix.terminfo` used `@`
(explicit cancellation — `dch1@`, `rmul@`, `blink@`, ...) for every
capability this port doesn't implement, instead of simply omitting them.
A cancelled string capability compiles to a distinct sentinel value
(`(char *)-1`) meaning "explicitly absent" — intended for un-setting a
capability a `use=` *parent* entry had, which this self-contained entry
(no `use=` anywhere) never needed. ncurses' own `newterm()` startup path
(`ncurses/base/lib_newterm.c`'s `SGR0_TEST` macro) checks a string
capability against `NULL` but not against that `-1` sentinel before
calling `strcmp()` on it — `rmul@` made it call `strcmp((char *)-1,
exit_attribute_mode)`, an instant fault on a bogus pointer. Confirmed by
temporarily instrumenting a scratch copy of the vendored source with a
proper `ebp`-chain stack walk in the kernel's own page-fault handler
(never committed — the pristine `third_party/ncurses/ncurses-6.5/` tree
is untouched) tracing `initscr → newterm → newterm_sp → strcmp`
precisely to that line. **Fix**: every unimplemented capability in
`pureunix.terminfo` is now simply omitted (see "The pureunix terminfo
entry" above) — no ncurses source was patched.

### 6. A real cross-compilation blind spot in ncurses' own `configure`

**Symptom**: `initscr()` succeeded, colors and cursor motion rendered
correctly, but arrow/function keys never worked inside any ncurses
program — every keypress came back as 2-3 separate raw bytes (`27`,
`91`, `66` for Down) instead of one `KEY_DOWN`, even though `has_key
(KEY_DOWN)` correctly reported `1` and `tigetstr("kcud1")` returned the
exact right 3-byte sequence.

**Root cause**: ncurses' `configure` (`aclocal.m4`'s `CF_FUNC_POLL`)
probes whether `poll()` *actually works* with a real `AC_TRY_RUN` — a
real pipe, a real `poll()` call, checked at runtime. That can't execute
while cross-compiling (the compiled probe is an `i686-elf` binary,
unrunnable on the host build machine), so autoconf silently falls back
to its pessimistic "unknown" cross-compiling answer, which — traced by
temporarily instrumenting a scratch copy of `ncurses/base/lib_getch.c`
and `ncurses/tty/lib_twait.c` with trace prints — permanently disables
`USE_FUNC_POLL` for the *entire* build, not just that one probe.
`kgetch()`'s escape-sequence trie matcher (`ncurses/base/lib_getch.c`)
calls `_nc_timed_wait()` between bytes of a suspected multi-byte key
sequence to check "is the rest of it available yet"; with
`USE_FUNC_POLL` off, `_nc_timed_wait()` never even calls `poll()` and
unconditionally reports "nothing more available", so `kgetch()` gives up
and returns the lone `ESC` byte after exactly one byte, every time.

**Fix**: `tools/build-ncurses.sh` pre-seeds the autoconf cache variable
`cf_cv_working_poll=yes` for the cross `configure` invocation — a
standard, well-supported technique for exactly this class of
cross-compilation gap (answering an un-runnable runtime probe with a
known-good answer instead of letting autoconf guess pessimistically).
This is not papering over a real limitation: PureUNIX's `poll()`
(`user/newlib_syscalls.c`) genuinely is correct for what `_nc_timed_wait`
needs here — every fd it's asked about is always truthfully reported
ready, and the `read()` that follows never blocks forever when data is
actually pending, which is everything this specific call site needs.

### 6b. `poll()`'s own doc comment was stale

Found in passing while investigating #6: `user/newlib_compat/poll.h`'s
header comment still said `poll()` isn't implemented ("linking a program
that actually calls it will fail") — stale from before it was
implemented for BusyBox's benefit. Not fixed as part of this port (out
of scope, purely cosmetic), but worth flagging for a future pass.

---

## Install layout

| On-disk path | Contents | Why there |
|---|---|---|
| `/bin/ncdemo.elf` | The ELF | `tools/mkext2.py`'s `add_bin()`, same as every other standalone ELF |
| `/bin/ncdemo` → `/bin/ncdemo.elf` | Symlink | Same pattern as `lua`/`sqlite3`/`neatvi`/`tty` — makes `ncdemo` resolve as an ordinary `$PATH` command |

`third_party/ncurses/i686-elf/include/` (`curses.h`, `term.h`, `panel.h`,
`menu.h`, `form.h`, `unctrl.h`, `ncurses_dll.h`, `termcap.h`) and
`third_party/ncurses/i686-elf/lib/` (`libncurses.a`, `libpanel.a`,
`libmenu.a`, `libform.a`) are available to any current or future
`NEWLIB_PROGRAMS`-style userspace binary via the Makefile's
`NCURSES_CFLAGS`/`NCURSES_LDFLAGS` (`-isystem .../include` / `-L
.../lib`), the same way `NEWLIB_CFLAGS`/`NEWLIB_LDFLAGS` already expose
newlib itself — no extra plumbing needed to link a *new* ncurses program
against the real library and headers.

EXT2-only (like BusyBox/TCC/Lua/SQLite) — FAT16 isn't part of the primary
ash userland.

---

## Testing

Verified interactively in QEMU against the real persistent disk image
(`build/pureunix.iso`, the exact `make iso` deliverable), via a new
`tools/test-ncurses-demo.py` combining the existing QMP `send-key`
driving technique (arrows, Enter, `q`, `Alt+F2`/`Alt+F1` VT switching)
with QMP `screendump` screenshots at each step — a full-screen redraw
doesn't produce a line-oriented transcript the way plain shell output
does, so this port's testing methodology needed a way to inspect the
actual rendered framebuffer, not just grep a serial log. (An early,
naive version of this test that only grepped the serial log for expected
substrings produced a **false pass** even while the kernel had already
panicked and halted — a static, frozen framebuffer trivially "matches
itself" before/after a VT switch. The final version screenshots and
visually confirms real, distinct, correctly-rendered content at every
step, not just structural pass/fail.)

- Real box-drawn windows, reverse-video menu selection, and all 16 ANSI
  colors (`COLOR_PAIR`s 1-7) render correctly and distinctly.
- The reported terminal size (`LINES`/`COLS`, `47x160` in QEMU's default
  framebuffer mode) is the real, dynamic, framebuffer-derived console
  size — not the terminfo source's placeholder `cols#80`/`lines#24`
  defaults, confirming `TIOCGWINSZ` is consulted correctly.
- Arrow keys (`Up`/`Down`/`Left`/`Right`/`Home`/`End`) drive real
  application navigation (menu selection, an on-screen cursor), each
  confirmed by an exact-position screenshot before/after a specific key
  sequence.
- **VT-switch safety**: `Alt+F2` away from a running `ncdemo`, then
  `Alt+F1` back — the screenshot taken immediately before the switch and
  immediately after returning are asserted **byte-identical** (not just
  visually similar), proving `kernel/vt.c`'s per-VT repaint-from-buffer
  path (`vga_console_repaint()`) neither corrupts a backgrounded
  ncurses screen nor loses/duplicates queued input.
- Clean shell restoration: after `q` quits `ncdemo` (`endwin()`), a
  plain `echo` at the shell prompt round-trips correctly — proves
  canonical mode/echo were correctly restored, not left in ncurses'
  raw/`noecho` state.
- Full regression suite re-verified with no *new* regressions:
  `libctest` 45/45, `exectest` 16/16, `dirtest` 14/14, `systest` 343/345
  (same 2 pre-existing console-geometry failures, `[129]`/`[130]` —
  unchanged; see project memory's `gotcha_preexisting_systest_failures`),
  `tools/test-persistent-boot.py` and `tools/test-sqlite-persistence.py`
  both still pass unchanged.

---

## Known gaps

- **No mouse support.** No mouse hardware/driver exists in this kernel at
  all; `pureunix.terminfo` correctly declares no mouse-reporting
  capability, so ncurses' mouse API compiles and links but has nothing
  to report.
- **No alternate screen buffer** (`smcup`/`rmcup`). Would need a second
  full-size cell buffer per console (`drivers/vga.c`'s `console_t`
  already carries ~120 KiB per console for the primary buffer alone;
  doubling that for every one of `VGA_MAX_CONSOLES` consoles was judged
  not worth the static-memory cost for this port's scope). A real
  terminal without this capability is a legitimate, well-precedented
  terminfo choice (e.g. plain `vt100`) — ncurses degrades to leaving
  `initscr()`'s clear on screen rather than restoring prior shell
  content on `endwin()`, which is what this port's testing already
  exercises and confirms is clean, not corrupted.
- **No underline/dim/blink rendering.** `drivers/font.c`'s bitmap glyphs
  have no underline/dim/blink variant to draw, so these SGR codes are
  harmless no-ops (matching real terminals that genuinely lack the
  hardware) and the corresponding terminfo capabilities are omitted, not
  declared.
- **No wide-character/UTF-8 support** (`--disable-widec`) — no Unicode
  glyph coverage in the bitmap font to render against.
- **`halfdelay()`/`VTIME`-based partial-second input timeouts don't
  really work** — `drivers/tty.c`'s raw-mode read blocks on the first
  byte regardless of `VTIME` (a pre-existing platform limitation, not
  new to this port; see `docs/syscalls.md`). `nodelay()`/non-blocking
  `wgetch()` mode also inherits this: a "no key available" nodelay
  poll can't distinguish from a genuinely blocking wait without a key
  actually being queued.
- **Real dynamic mid-session resize is a narrow case.** `SIGWINCH` fires
  correctly on the one existing runtime geometry-change path (font
  scale), but there's no window manager or terminal emulator resizing
  PureUNIX's own framebuffer console interactively the way a desktop
  terminal emulator would — the mechanism is real and tested, but the
  triggering scenario is inherently rare on this platform.
