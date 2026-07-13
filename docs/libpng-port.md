# libpng Port

## Overview

Real, unmodified [libpng](http://www.libpng.org/pub/png/libpng.html) 1.6.58
runs natively as a userspace static library (`/usr/lib/libpng16.a`,
`/usr/include/png.h`+`pngconf.h`+`pnglibconf.h`) on PureUNIX, built directly
against the real zlib port (`docs/zlib-port.md`) — the real upstream PNG
codec, not a project-authored decoder. It exists to back `imgview`
(`docs/imgview.md`), which links against it directly and uses libpng's own
transform APIs to normalize any supported PNG into 8-bit RGBA.

## Architecture

### Real upstream source, no upstream patches

`third_party/libpng/libpng-1.6.58/` is an unmodified extraction of the
upstream release tarball — same "vendor upstream source, compile it with
our own Makefile rules, no upstream build system" pattern as every other
`third_party/` port. The 15 real library source files (`png.c`,
`pngerror.c`, `pngget.c`, `pngmem.c`, `pngpread.c`, `pngread.c`, `pngrio.c`,
`pngrtran.c`, `pngrutil.c`, `pngset.c`, `pngtrans.c`, `pngwio.c`,
`pngwrite.c`, `pngwtran.c`, `pngwutil.c` — upstream's own
`libpng16_la_SOURCES` list, minus platform-specific SIMD files this generic
i686 target doesn't use: `arm/`, `intel/`, `mips/`, `powerpc/`, `loongarch/`,
`riscv/`) compiled cleanly against newlib on the first try — no libc gaps
at all, unlike zlib's one small `unistd.h` fix. `example.c`/`pngtest.c` are
upstream's own demo/self-test programs, not part of the library, and
aren't compiled.

### `pnglibconf.h`: upstream's own documented no-`configure` escape hatch

libpng's own `./configure` mainly exists to generate `pnglibconf.h` (which
options are compiled in) from `scripts/pnglibconf.dfa` via `awk` — a real
build step this project's "no upstream build system" pattern deliberately
avoids running. libpng's own `INSTALL` file documents exactly this
situation and gives the fix directly:

```
cp scripts/pnglibconf.h.prebuilt pnglibconf.h
cp scripts/makefile.system makefile
```

`scripts/pnglibconf.h.prebuilt` is "the configuration used to build the
distributed dll and lib files" per that same doc — a real, upstream-
shipped artifact, not a project invention, in the same spirit as ncurses'
own prebuilt-and-vendored build output (`third_party/ncurses/README.md`)
or htop's hand-written `config.h` standing in for what `configure` would
otherwise generate. The Makefile copies it into the *build* tree
(`$(BUILD)/user/libpng/pnglibconf.h`) at build time — never into the
vendored `third_party/libpng/` source tree itself, so the vendored copy
stays a byte-for-byte unmodified upstream extraction. It enables every
transform `imgview` needs by default (`PNG_READ_EXPAND_SUPPORTED`,
`PNG_READ_GRAY_TO_RGB_SUPPORTED`, `PNG_READ_FILLER_SUPPORTED`,
`PNG_READ_INTERLACING_SUPPORTED`, `PNG_READ_STRIP_16_TO_8_SUPPORTED`, ...)
and disables every CPU-specific SIMD path (`PNG_INTEL_SSE`, `PNG_ARM_NEON`,
...) this generic i686 build doesn't compile in anyway.

### Build integration (`Makefile`)

`LIBPNG_SRC`/`LIBPNG_BUILD`/`LIBPNG_SRCS`/`LIBPNG_CFLAGS` compile the 15
source files into `$(BUILD)/user/libpng/*.o` (each depending on the
generated `pnglibconf.h` above), archived into
`$(BUILD)/user/libpng/libpng16.a` (`LIBPNG_LIB`) — the same per-file-
compile-then-`ar rcs` pattern zlib/SDL2 use. `LIBPNG_CFLAGS` puts
`$(ZLIB_SRC)` on the include path so `png.h`'s `#include <zlib.h>` resolves
directly against the real vendored zlib headers, not a copy.

### On-disk headers/libs for future ports

Same `ZLIB_LIBPNG_EXTRA_FILES` mechanism as the zlib port: real `png.h`,
`pngconf.h`, the generated `pnglibconf.h`, and `libpng16.a` are installed
onto the EXT2 image at `/usr/include`/`/usr/lib`, so a future
`third_party/`-style Makefile port or a bare on-target
`tcc foo.c -lpng16 -lz` can compile and link against real libpng.

## Testing

- **Direct decode correctness** (`docs/imgview.md`'s Testing section has
  the full detail): `tools/gen-test-pngs.py` hand-builds real, valid PNG
  files (verified independently against macOS's own `sips`/ImageIO
  decoder — a completely different PNG implementation from libpng/zlib —
  confirming correct dimensions and alpha-channel presence before ever
  touching the target) covering RGB (color type 2), RGBA (color type 6,
  with a genuine alpha ramp), a tiny image, and a 2000×1200 image forcing
  the downscale path. `imgview` decodes every one of them with libpng's
  real `png_read_info`/`png_set_*` transform/`png_read_image`/`png_read_end`
  sequence, and a QEMU framebuffer screendump is compared pixel-by-pixel
  against the exact predicted output — not just "did it exit 0".
- **libpng's own transform pipeline, exercised for real**:
  `png_set_palette_to_rgb`, `png_set_expand_gray_1_2_4_to_8`,
  `png_set_tRNS_to_alpha`, `png_set_strip_16`, `png_set_gray_to_rgb`,
  `png_set_add_alpha`, and `png_set_interlace_handling` (Adam7
  de-interlacing) are all wired up in `user/imgview.c`'s `load_png()` —
  the standard "normalize any PNG to 8-bit RGBA" recipe from libpng's own
  manual, not a bespoke conversion.
- `make run-test` (`user/systest.c` regression suite) re-run clean after
  this port: 343/345, the same 2 pre-existing failures as before.
