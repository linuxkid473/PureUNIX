# regex (vendored)

The classic 4.4BSD/Henry Spencer POSIX regex engine (`regcomp`/`regexec`/
`regerror`/`regfree`), vendored from NetBSD's current source tree
(`lib/libc/regex/`, `$NetBSD: ... 2025$` timestamps in each file's header
comment) — same lineage as the `<regex.h>` newlib already ships
(`third_party/newlib/i686-elf/include/regex.h`, itself a slightly older copy
of this same code), so `regex_t`/`regmatch_t`'s layout needs no adaptation.
Vendored as source (unlike `third_party/newlib`/`third_party/busybox`'s
prebuilt artifacts) because it's small (~2500 lines across 5 `.c`/4 `.h`
files) and this project's own Makefile compiles it directly, the same
pattern `user/vi/` (Neatvi) uses.

newlib has no `regcomp()`/`regexec()` *implementation* at all for this bare
target — only the header — which is what left `grep`/`egrep`/`fgrep`
disabled in earlier phases of the BusyBox port (see project docs/memory).
This fills that gap.

## Files

- `regcomp.c`, `regexec.c`, `regerror.c`, `regfree.c`, `engine.c`,
  `regex2.h`, `cname.h`, `utils.h` — upstream NetBSD source, byte-for-byte.
- `COPYRIGHT` — upstream's own license file (3-clause BSD).
- `namespace.h` — **not** upstream. NetBSD's real `namespace.h` renames
  every libc symbol here into a reserved-namespace alias so this code can
  live inside NetBSD's own libc without clashing with an application's own
  `regcomp()`; PureUNIX has no such symbol, so this is a small local stub
  instead, quote-included exactly where NetBSD's real one would be
  (`#include "namespace.h"`, resolved relative to each vendored `.c` file
  per normal C quote-include search order — no build system changes
  needed). It supplies the couple of NetBSD-internal macros newlib's
  `sys/cdefs.h` doesn't define: `_DIAGASSERT` (a no-op here — these are
  debug-build assertions, and this code is never built with `REDEBUG`),
  `__UNCONST`, `__arraycount`.

## Build configuration

Compiled **without** `NLS` defined (see `utils.h`): no locale/wide-character
support, ordinary `<ctype.h>` byte classification only. This is deliberate,
not a limitation worth chasing — BusyBox itself only ever runs in the C
locale here (no locale data exists on PureUNIX), and it drops the need for
`mbrtowc()`/`wcscoll()`/real `iswctype()` entirely (confirmed by grep: every
reference to those is inside `#ifdef NLS` in the vendored sources). Also not
defined: `REGEX_LIBC_COLLATE` (NetBSD-libc-internal collation tables that
don't exist here — falls back to plain byte comparison for `[a-z]`-style
ranges, correct for the C locale) and `HAVE_NBTOOL_CONFIG_H`/`LIBHACK`
(NetBSD build-system-internal, not applicable).

`<regex.h>` itself is **not** here — see `user/newlib_compat/regex.h`
instead (a current NetBSD copy, shadowing newlib's older FreeBSD-8.2-vintage
one via the usual `-isystem` precedence), because this code needs the
`REG_GNU`/`REG_STARTEND`/`REG_TRACE` flags the newer header adds that
newlib's copy predates. Both headers are ABI-compatible (`regex_t` hasn't
changed shape since 4.4BSD); any other program that happens to
`#include <regex.h>` in this repo now sees the same, newer header.

## Rebuilding

There is no separate rebuild script — these files are compiled directly by
the top-level `Makefile`/`tools/build-busybox.sh` alongside BusyBox's own
sources (see `REGEX_SRCS`/`REGEX_OBJS` in the Makefile). To pick up an
upstream fix, re-fetch the individual files from
`https://raw.githubusercontent.com/NetBSD/src/trunk/lib/libc/regex/<file>`
and `https://raw.githubusercontent.com/NetBSD/src/trunk/include/regex.h`
(the latter goes to `user/newlib_compat/regex.h`, with the
`<sys/featuretest.h>` include dropped — see that file's own header comment).
