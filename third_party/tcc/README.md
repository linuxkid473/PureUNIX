# TinyCC / TCC (vendored)

[TinyCC](https://bellard.org/tcc/) 0.9.27 source
(`https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2`),
vendored as source (not a prebuilt binary — unlike `third_party/newlib` and
`third_party/busybox`) so its small, targeted portability patches stay
visible and diffable against upstream. TCC itself is compiled by PureUNIX's
own top-level `Makefile` (see `TCC_*` there), not TCC's own `configure`/
`Makefile`/`lib/Makefile` — see "Why not TCC's own build system" below.
`win32/` (TCC's Windows/PE support tree, irrelevant to an i386 ELF target)
was dropped from the vendored copy; everything else is an unmodified
extraction of the upstream release tarball plus the patches listed here.

`COPYING` here is TCC's own license (LGPL 2.1); `RELICENSING` documents
upstream's move toward a more permissive license for some contributions —
both carried over unmodified from the release tarball.

## Why not TCC's own build system

TCC's `./configure` fingerprints the *build host* via `uname` (this repo is
built on macOS/Linux, cross-compiling *for* PureUNIX) — it has no case for
PureUNIX at all, and would wire in whatever host-specific defaults it
guesses (e.g. `Darwin` → OSX-flavored `.dylib` settings), all wrong for an
i386 ELF target. Since TCC's default build is already an amalgamated
single-translation-unit build (`ONE_SOURCE=1` in `tcc.h`), reproducing it
with two plain `$(CC) -c` rules in PureUNIX's own Makefile (`libtcc.c`
being the amalgamated core — `tccpp.c`/`tccgen.c`/`tccelf.c`/`tccasm.c`/
`tccrun.c`/`i386-{gen,link,asm}.c` are all `#include`d into it — and
`tcc.c`, the CLI frontend, built with `-DONE_SOURCE=0`) is simpler and
fully within PureUNIX's own control, the same approach already used for
vendored source trees like `user/vi/` (Neatvi). See `docs/tcc-port.md` for
the exact Makefile flags/defines used.

## Patches applied (all upstreamable-style portability fixes, gated behind
a new `TCC_PUREUNIX` define — the same pattern `CONFIG_OSX`/`TCC_UCLIBC`/
`TCC_MUSL` already use in this exact codebase for other targets)

1. **`tcc.h`**: `CONFIG_TCC_BACKTRACE`/`CONFIG_TCC_BCHECK` (the `-run`
   crash backtrace and bounds-checker fault handler) are no longer
   auto-enabled when `TCC_PUREUNIX` is defined. Both exist to catch
   `SIGSEGV`/`SIGFPE` — PureUNIX delivers neither (no signal-delivery
   mechanism exists yet, see `docs/syscalls.md`), so this code could never
   fire, but would still need a glibc-shaped `<sys/ucontext.h>` (`REG_EIP`,
   `uc_mcontext.gregs[]`, ...) to *compile*. Excluding it entirely avoids
   needing a fake ucontext shim for functionality that can't work anyway.

2. **`libtcc.c`** (`tcc_new()`): defaults `s->static_link = 1` when
   `TCC_PUREUNIX` is defined. Upstream's own default is *dynamic* linking
   (a plain `tcc foo.c` emits `PT_INTERP`/`PT_DYNAMIC` unless `-static` is
   passed — see `tccelf.c`), but PureUNIX has no dynamic linker at all;
   left at the upstream default, `tcc hello.c` with no flags would produce
   a binary with unresolved PLT/GOT relocations. `-static` remains
   available and is now simply the (idempotent) default.

3. **`config.h`** (new file, not a patch to an existing one): hand-written
   in place of `./configure`'s generated output — see that file's own
   header comment. Only defines `TCC_VERSION`; every path/target define
   (`CONFIG_TCC_CRTPREFIX`, `CONFIG_TCC_SYSINCLUDEPATHS`,
   `CONFIG_TCC_LIBPATHS`, `CONFIG_TCC_ELFINTERP`, `TCC_TARGET_I386`,
   `TCC_PUREUNIX`, `CONFIG_TCC_STATIC`) comes from the top-level
   Makefile's `-D` flags instead (`tcc.h` guards each with `#ifndef`, so
   this is no different from a real `config.h` being overridden).

No other source files are modified. `libtcc1.a` (TCC's runtime helper
library — 64-bit divide/shift, float↔int conversion helpers) is built
directly from `lib/libtcc1.c` + `lib/alloca86.S`/`alloca86-bt.S` with
`i686-elf-gcc` (TCC's own `lib/Makefile` supports this via
`i386-libtcc1-usegcc=yes`, sidestepping the usual tcc-builds-its-own-libtcc1
bootstrap) — see `docs/tcc-port.md`.

See `docs/tcc-port.md` for the full sysroot layout, install paths, and
every incompatibility found while bringing this up.
