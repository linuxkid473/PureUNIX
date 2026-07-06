/* Shadow for newlib's own <stdio.h> (third_party/newlib) — real header,
 * genuine gap: newlib only exposes this function as __getline() (see that
 * header), never under its public POSIX.1-2008 name getline(), which
 * include/platform.h assumes exists (HAVE_GETLINE 1) regardless of target.
 * Signatures match exactly, so a macro alias is enough — no wrapper
 * function needed.
 *
 * #include_next finds the next <stdio.h> on the include path — i.e.
 * newlib's real one — since this file's directory (user/newlib_compat) is
 * searched first (see the Makefile's NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_STDIO_H
#define PUREUNIX_NEWLIB_COMPAT_STDIO_H

#include_next <stdio.h>

#define getline __getline

#endif
