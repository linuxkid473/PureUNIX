# ncurses (vendored)

[ncurses](https://invisible-island.net/ncurses/) 6.5
(`https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz`, sha256
`136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6`), vendored
as an unmodified extraction of the upstream release tarball under
`ncurses-6.5/` — every file there is byte-for-byte identical to what
`invisible-island.net`/`ftp.gnu.org` ship. No ncurses source file is patched
for this port.

`i686-elf/` is a second, *built* artifact: real headers (`curses.h`,
`term.h`, `panel.h`, `menu.h`, `form.h`, ...) and real static libraries
(`libncurses.a`, `libpanel.a`, `libmenu.a`, `libform.a`) produced by actually
running ncurses' own `configure`/`make` — cross-compiled for `i686-elf` with
the same `i686-elf-gcc` and newlib flags every other PureUNIX userspace
binary uses — then vendored and committed, exactly the way
`third_party/newlib/` vendors a *built* newlib instead of newlib source. See
`docs/ncurses-port.md` for why (short version: ncurses generates a
substantial amount of its own source at build time — `curses.h`/`term.h`
from templates, capability tables from its `Caps` database via `awk`
scripts, and a compiled-in terminfo table from `pureunix.terminfo` below via
its own `tic` — hand-reproducing that with PureUNIX-authored Makefile rules,
the way `third_party/lua/`'s object list does, would mean re-deriving
hundreds of generated lines by hand instead of vendoring them).

`pureunix.terminfo` here is PureUNIX's own terminfo *source* description —
a new terminal type, not a patch to any ncurses file — describing
`drivers/vga.c`'s ANSI/VT100-subset escape-sequence interpreter and
`drivers/tty.c`'s keyboard input encoding capability-by-capability. It gets
appended to a scratch copy of ncurses' own `misc/terminfo.src` (which is
*itself* nothing but ~4000 entries in exactly this format) and compiled
straight into `libncurses.a` via `--with-fallbacks=pureunix,dumb`, since
PureUNIX has no terminfo database on its filesystem at all
(`--disable-database`).

To rebuild `i686-elf/` (e.g. to bump the ncurses version, or after editing
`pureunix.terminfo`), run `tools/build-ncurses.sh` from the repo root and
commit the result — same workflow as `tools/build-newlib.sh`.

`LICENSE` here is ncurses' own MIT-style license, copied verbatim from
`ncurses-6.5/COPYING`.
