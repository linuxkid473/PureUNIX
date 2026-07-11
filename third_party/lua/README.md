# Lua (vendored)

[Lua](https://www.lua.org) 5.4.7 source
(`https://www.lua.org/ftp/lua-5.4.7.tar.gz`, sha256
`9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`), vendored
as an unmodified extraction of the upstream release tarball — every file
under `lua-5.4.7/` is byte-for-byte identical to what `lua.org` ships. No
Lua source file is patched for this port; see "Why no patches were needed"
below. `LICENSE` here is Lua's MIT license, copied verbatim out of the
"License" section of `lua-5.4.7/doc/readme.html` (Lua's release tarball has
never shipped a standalone `LICENSE` file — the license text only exists
embedded in the HTML docs — so this is a plain-text extraction, not a
PureUNIX addition).

Lua is built directly by PureUNIX's own top-level `Makefile` (see the `Lua
5.4.7` section there), the same "vendor upstream source, compile it with our
own Makefile rules, no upstream build system" pattern already used for
`third_party/tcc/` and `user/vi/` (Neatvi) — Lua's own `src/Makefile` isn't
used at all (see "Why not Lua's own Makefile" below). Two ELF binaries come
out of this: `/bin/lua` (the interactive REPL / script runner) and
`/bin/luac` (the standalone bytecode compiler), both installed on the
persistent EXT2 image (`docs/lua-port.md`).

## Why not Lua's own Makefile

Lua's `src/Makefile` `PLATS` targets (`linux`, `macosx`, `bsd`, `posix`, ...)
each hardcode a specific *hosted* compiler/platform combination and, more
importantly, none of them describe PureUNIX at all — there is no
`PLAT=pureunix` to add without patching the Makefile itself, and the whole
point of vendoring source (rather than a prebuilt binary, unlike
`third_party/newlib`/`third_party/busybox`) is to keep upstream untouched.
Lua's actual build is trivial by design (compile every `.c` in `src/` except
`lua.c`/`luac.c` into a library, then link each of those two against it) —
reproducing it with a handful of `$(CC) -c`/link rules in PureUNIX's own
Makefile, using the *exact* `CORE_O`/`LIB_O` object lists Lua's own
`src/Makefile` defines (so this stays a faithful reproduction, not a
guess at what Lua needs), was simpler and kept fully within PureUNIX's
own build rather than adding a second build system to shell out to.

## Why no patches were needed

Unlike TCC (three small `TCC_PUREUNIX`-gated patches — see
`third_party/tcc/README.md`), Lua's own portability layer already has an
exact fit for PureUNIX with zero source changes, just a `-D` flag:

- **`-DLUA_USE_POSIX`** takes `luaconf.h` down its already-existing POSIX
  configuration branch: `popen()`/`pclose()` for `io.popen`, `fseeko()`/
  `ftello()` for large-file seeks, `getc_unlocked()`/`flockfile()`/
  `funlockfile()` for buffered reads, `sigaction()`-based Ctrl-C handling
  in the `lua.c` CLI, and `isatty()`-based interactive-session detection.
  Every one of these is now backed by a *real* PureUNIX syscall or newlib
  addition (`user/newlib_syscalls.c` — see `docs/lua-port.md`'s
  "Platform work this needed" for exactly what had to be added/fixed),
  not a stub — this is a real POSIX build of Lua, not a crippled one.
- **`LUA_USE_DLOPEN` is deliberately left undefined.** PureUNIX has no
  dynamic linker at all (the same reasoning as TCC's `CONFIG_TCC_STATIC`
  default), so `loadlib.c` falls through to its own upstream "Fallback for
  other systems" branch (`#else` in that file's dl-backend selection) —
  a real, complete, upstream-provided implementation that makes
  `package.loadlib`/`require` of a compiled `.so` module fail cleanly with
  `"dynamic libraries not enabled"`, exactly as it would on any other
  statically-linked-only Lua target. `require()` of ordinary `.lua` source
  modules (via `LUA_LDIR`/`LUA_CDIR`, `package.path`) is unaffected and
  works fully — see `docs/lua-port.md`.
- **`LUA_ROOT`** (`luaconf.h`'s non-Windows branch) already defaults to
  `/usr/local/` with no override needed — `LUA_LDIR`/`LUA_CDIR` resolve to
  `/usr/local/share/lua/5.4/` and `/usr/local/lib/lua/5.4/`, both real,
  populated directories on PureUNIX's EXT2 image (`tools/mkext2.py`'s
  `add_lua()`) — an entirely ordinary Unix install layout, not a
  PureUNIX-specific path.
- **`LUA_COMPAT_5_3`** is carried over from Lua's own `src/Makefile`
  default (harmless backward-compatibility shims), not a PureUNIX
  addition.

`doc/` (the full upstream HTML manual, `lua.1`/`luac.1` man pages) is kept
unmodified alongside `src/` for reference even though nothing in the build
reads it — the same "keep upstream intact" reasoning as the rest of this
vendoring.
