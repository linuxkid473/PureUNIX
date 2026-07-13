# libpng (vendored)

Real [libpng](http://www.libpng.org/pub/png/libpng.html) 1.6.58 source
(`https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz`),
vendored as an unmodified extraction of the upstream release — every file
under `libpng-1.6.58/` is byte-for-byte identical to what upstream ships.
No libpng source file is patched for this port.

Built directly by PureUNIX's own top-level `Makefile` (see the "libpng
1.6.58" section there) against the real vendored zlib port
(`third_party/zlib/`), the same "vendor upstream source, compile it with
our own Makefile rules, no upstream build system" pattern as every other
`third_party/` port. One static library comes out of this:
`/usr/lib/libpng16.a`, installed on the persistent EXT2 image alongside
real `png.h`/`pngconf.h`/`pnglibconf.h` headers (`docs/libpng-port.md`) —
the real dependency behind `/bin/imgview` (`docs/imgview.md`).

## Why not libpng's own `./configure`

libpng's own `./configure` mainly exists to generate `pnglibconf.h` (which
optional features/transforms are compiled in) from
`scripts/pnglibconf.dfa` via `awk`. libpng's own `INSTALL` file documents
a real, upstream-supported alternative for exactly this "no configure"
situation:

```
cp scripts/pnglibconf.h.prebuilt pnglibconf.h
```

`pnglibconf.h.prebuilt` is "the configuration used to build the
distributed dll and lib files" per that same file — a real artifact
upstream ships, not a project invention (the same spirit as ncurses'
prebuilt-and-vendored build output, or htop's hand-written `config.h`
standing in for what `configure` would otherwise generate). PureUNIX's
Makefile copies it into the *build* tree at build time — never into this
vendored source tree, which stays a byte-for-byte unmodified extraction.

No SIMD-optimized platform sources (`arm/`, `intel/`, `mips/`, `powerpc/`,
`loongarch/`, `riscv/`) are compiled — the prebuilt config doesn't enable
any of them by default, and this is a generic i686 target with no
runtime CPU-feature dispatch.

See `docs/libpng-port.md` for the full architecture and how this was
tested (real PNG fixtures, decoded and screendump-verified pixel-by-pixel
against predicted output through `imgview`).
