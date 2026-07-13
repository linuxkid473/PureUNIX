# zlib Port

## Overview

Real, unmodified [zlib](https://zlib.net) 1.3.2 runs natively as a userspace
static library (`/usr/lib/libz.a`, `/usr/include/zlib.h`+`zconf.h`) on
PureUNIX — the real upstream compression/decompression codec, not a
simplified stand-in or a project-authored DEFLATE implementation. It exists
to be libpng's real dependency (`docs/libpng-port.md`), and through libpng,
`imgview`'s (`docs/imgview.md`) — the whole point of this port is a real
`inflate()` decoding real PNG `IDAT` data on PureUNIX, not merely a library
that compiles.

```
# /bin/ziptest.elf
zlib version: 1.3.2
repetitive text: PASS (140 -> 73 -> 140 bytes)
empty buffer: PASS (0 -> 8 -> 0 bytes)
8KiB pseudo-random data: PASS (8192 -> 8203 -> 8192 bytes)
crc32(text) = 0xadd407b5
adler32(text) = 0xfa063052
ziptest: ALL PASS
```

## Architecture

### Real upstream source, no upstream patches, no `./configure`

`third_party/zlib/zlib-1.3.2/` is an unmodified extraction of the upstream
release tarball — same "vendor upstream source, compile it with our own
Makefile rules, no upstream build system" pattern as TCC/Lua/SQLite/htop/
SDL2/Chocolate Doom. Unlike every one of those, there isn't even a
`./configure`-generated artifact being stood in for here: zlib's own
`./configure` only ever regenerates a `Makefile` and (redundantly) `zconf.h`
from `zconf.h.in`, and the release tarball already ships both pre-generated
identically (`diff zconf.h zconf.h.in` is empty) — so the vendored tree is
compiled exactly as extracted, with zero generated files.

### The one real gap: `-DHAVE_UNISTD_H=1`

`gzlib.c`/`gzread.c`/`gzwrite.c` (the real gzip-file-I/O layer, `gzopen()`/
`gzread()`/`gzwrite()`/...) call real `read()`/`write()`/`close()`/`lseek()`
on the underlying file descriptor. `zconf.h` only declares those (via
`#include <unistd.h>`) when `Z_HAVE_UNISTD_H` is set, which it only ever is
from a real `./configure`'s `HAVE_UNISTD_H` substitution or a couple of
hardcoded compiler checks (Watcom, DJGPP) that don't apply to this target —
without it, those three files fail to compile with "implicit declaration of
function" errors. Fixed the same way SQLite's `-DHAVE_USLEEP=1` declares a
real, true platform capability instead of patching source: newlib provides
a real, complete POSIX `<unistd.h>` here, so `ZLIB_CFLAGS` in the top-level
Makefile just says so directly (`-DHAVE_UNISTD_H=1`). Every other one of
zlib's 15 core source files (`adler32.c`, `compress.c`, `crc32.c`,
`deflate.c`, `gzclose.c`, `infback.c`, `inffast.c`, `inflate.c`,
`inftrees.c`, `trees.c`, `uncompr.c`, `zutil.c`) compiled cleanly against
newlib on the first try — no other libc gap.

### Build integration (`Makefile`)

`ZLIB_SRC`/`ZLIB_SRCS`/`ZLIB_OBJS`/`ZLIB_CFLAGS` compile each of the 15
`.c` files into `$(BUILD)/user/zlib/*.o`, archived into
`$(BUILD)/user/zlib/libz.a` (`ZLIB_LIB`) — the same per-file-compile-then-
`ar rcs` pattern SDL2's `libSDL2.a` uses. Any program that needs zlib links
against `$(ZLIB_LIB)` the same way `$(SDLTEST_ELF)` links against
`$(SDL_LIB)`; libpng and imgview both do (see their own docs).

### On-disk headers/libs for future ports

`ZLIB_LIBPNG_EXTRA_FILES` installs the real `zlib.h`/`zconf.h`/`libz.a`
(and libpng's own headers/lib) onto the EXT2 image at `/usr/include`,
`/usr/lib` — the same paths `TCC_SYSROOT_FILES` already populates with
newlib's own headers/`libc.a` for TCC's on-target `tcc foo.c` to find with
zero flags. So both a future `third_party/`-style Makefile port (via
`$(ZLIB_SRC)`/`$(ZLIB_LIB)` directly, the same `NCURSES_DIR`-style pair
this port itself uses) and a bare on-target `tcc foo.c -lz` can compile and
link against real zlib.

## Testing

- **`/bin/ziptest.elf`** (`user/ziptest.c`, installed as a permanent
  regression-test binary, not an end-user command — no `/bin` symlink,
  same category as `systest.elf`/`opentest.elf`): a direct `compress()`/
  `uncompress()` roundtrip over three payload shapes (repetitive text, an
  empty buffer, 8 KiB of pseudo-random effectively-incompressible data —
  exercising deflate's near-1:1 "stored block" path, not just the easy
  repetitive case) plus `crc32()`/`adler32()` sanity checks. Run under
  QEMU against the real `build/pureunix.iso`: all three roundtrips and both
  checksums passed, and the `crc32`/`adler32` values were independently
  cross-checked against CPython's own (unrelated) zlib binding on the host
  — byte-for-byte identical (`0xadd407b5` / `0xfa063052`).
- **Indirect, and arguably stronger, proof via real PNG decode**
  (`docs/imgview.md`/`docs/libpng-port.md`'s Testing sections): every PNG
  `IDAT` chunk is itself DEFLATE-compressed, and `tools/gen-test-pngs.py`'s
  fixtures were compressed on the *host* with Python's own zlib binding —
  so a QEMU screendump showing the *exact* predicted decoded pixel value at
  a sampled coordinate (done for 4 real PNGs, including a 2000×1200/~925 KB
  one with substantial compressed data) is a real, end-to-end proof that
  target-side `inflate()` correctly reverses host-side `zlib.compress()` on
  genuine compressed data, not merely that the two link together.
- `make run-test` (the existing `user/systest.c` regression suite via
  `tools/vt-inject-test.py`) was re-run after this port: 343/345 passed,
  the same 2 pre-existing console-geometry failures as before — no new
  regressions.
