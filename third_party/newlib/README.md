# newlib (vendored)

Prebuilt [newlib](https://sourceware.org/newlib/) 4.6.0.20260123, cross-compiled
for the bare-metal `i686-elf` target with the same `i686-elf-gcc` used to build
the rest of PureUNIX. Vendored (rather than built from source on every `make`)
so the build stays network-free and reproducible.

- `i686-elf/include/` — full newlib C header set (stdio.h, stdlib.h, string.h, math.h, ...)
- `i686-elf/lib/libc.a`, `i686-elf/lib/libm.a` — the compiled libc and libm

newlib has no built-in support for a `i686-*-elf` OS (see its
`configure.host`), so it does not provide POSIX-named syscall wrappers
(`open`, `read`, `write`, ...) on its own — it only calls them. This build
was configured with `-DMISSING_SYSCALL_NAMES`, which makes newlib's internal
reentrant layer call those plain POSIX names directly. PureUNIX supplies them
in `user/newlib_syscalls.c`, translating to `user/libpure.h`'s `pu_*` syscall
wrappers (`int $0x80`). See `docs/userland.md` for how a newlib-linked program
is put together.

To rebuild this vendored copy (e.g. to bump the newlib version), run
`tools/build-newlib.sh` from the repo root and commit the result.

`LICENSE` here is newlib's own `COPYING.NEWLIB` (BSD-style; a handful of
files inside newlib carry their own, compatible licenses — see that file).
