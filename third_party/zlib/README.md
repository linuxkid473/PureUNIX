# zlib (vendored)

Real [zlib](https://zlib.net) 1.3.2 source (`https://zlib.net/zlib-1.3.2.tar.gz`),
vendored as an unmodified extraction of the upstream release tarball —
every file under `zlib-1.3.2/` is byte-for-byte identical to what
zlib.net ships. No zlib source file is patched for this port.

Built directly by PureUNIX's own top-level `Makefile` (see the "zlib
1.3.2" section there), the same "vendor upstream source, compile it with
our own Makefile rules, no upstream build system" pattern already used for
`third_party/tcc/`, `third_party/lua/`, `third_party/sqlite/`. One static
library comes out of this: `/usr/lib/libz.a`, installed on the persistent
EXT2 image alongside real `zlib.h`/`zconf.h` headers (`docs/zlib-port.md`).

## Why not zlib's own `./configure`

Unlike TCC/Lua/SQLite (where `-D` flags stand in for what a real
`./configure` would generate) there isn't even a generated artifact to
stand in for here: zlib's own `./configure` only ever regenerates a
`Makefile` and — redundantly — `zconf.h` from `zconf.h.in`. The release
tarball already ships both pre-generated identically (`diff zconf.h
zconf.h.in` is empty), so this port compiles the vendored tree exactly as
extracted, with zero generated files of any kind.

## Compile-time configuration

Exactly one `-D` flag, the same "declare a real, true platform capability
explicitly" style as SQLite's `-DHAVE_USLEEP=1`:

| Define | Purpose |
|---|---|
| `HAVE_UNISTD_H=1` | `gzlib.c`/`gzread.c`/`gzwrite.c` (the gzip-file-I/O layer) call real `read()`/`write()`/`close()`/`lseek()`. `zconf.h` only declares those via `<unistd.h>` when `Z_HAVE_UNISTD_H` is set, which only ever happens from a real `./configure`'s substitution or a couple of hardcoded compiler checks that don't apply here. newlib provides a real, complete POSIX `<unistd.h>` on this target, so this just says so directly instead of patching source. |

See `docs/zlib-port.md` for the full architecture and how this was tested
(a direct `compress()`/`uncompress()` roundtrip regression binary, plus
real end-to-end proof via libpng/imgview actually decoding real PNG files).
