# htop (vendored)

[htop](https://htop.dev) 3.5.1
(`https://github.com/htop-dev/htop/releases/download/3.5.1/htop-3.5.1.tar.xz`,
sha256
`526cecd62870aa8d14d2a79a35ea197e4e2b5317d275b567cee0574b2ddb2e9a`),
vendored as an unmodified extraction of the upstream release tarball —
every file under `htop-3.5.1/` is byte-for-byte identical to what
`htop.dev`/GitHub Releases ship. No htop source file is patched for this
port.

`pureunix/` is a new htop *platform backend* — `Platform.c`,
`PureUnixMachine.c`, `PureUnixProcess.c`, `PureUnixProcessTable.c`,
`ProcessField.h`, and a hand-written `config.h` standing in for the one
htop's own `configure` would otherwise generate — following the exact
same plugin contract every other platform directory (`htop-3.5.1/linux/`,
`htop-3.5.1/freebsd/`, ...) already implements, reading PureUNIX's own
real `/proc` (`fs/procfs.c`). This is PureUNIX-authored code, not
vendored upstream — see `docs/htop-port.md` for the full architecture and
`pureunix/config.h`'s own header comment for why a hand-written
`config.h` replaces running htop's own `configure`.

htop is built directly by PureUNIX's own top-level `Makefile` (see the
`htop` section there), the same "vendor upstream source, compile it with
our own Makefile rules, no upstream build system" pattern already used
for `third_party/tcc/`/`third_party/lua/`/`third_party/sqlite/` — htop's
own `configure`/`Makefile.am` only ever *selects* which platform
subdirectory's `.c` files join one fixed, hand-enumerable core file list
(`Makefile.am`'s `myhtopsources`), which is reproduced directly in
PureUNIX's Makefile instead. One ELF binary comes out of this:
`/bin/htop`, the real upstream interactive process monitor, linked
against the real ported ncurses (`third_party/ncurses/`,
`docs/ncurses-port.md`) and installed on the persistent EXT2 image
(`docs/htop-port.md`).

`LICENSE` here is htop's own GNU GPLv2, copied verbatim from
`htop-3.5.1/COPYING`.
