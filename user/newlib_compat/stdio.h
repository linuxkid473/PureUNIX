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
 *
 * C++-only guard: libstdc++'s own <bits/basic_string.tcc> declares an
 * unrelated internal template also named __getline (std::getline's helper).
 * A blind textual #define getline __getline renames std::getline's own
 * declarations/definitions inconsistently depending on include order,
 * producing "template-id '__getline<>' ... does not match any template
 * declaration". No C++ code needs this C-library alias (C++ callers use
 * std::getline), so it's scoped to C translation units only.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_STDIO_H
#define PUREUNIX_NEWLIB_COMPAT_STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <stdio.h>

#ifndef __cplusplus
#define getline __getline
#endif


#ifdef __cplusplus
}
#endif
#endif
