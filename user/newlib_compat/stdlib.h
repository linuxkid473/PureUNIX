/* Shadow for newlib's own <stdlib.h> (third_party/newlib) — unlike most
 * headers in this directory, newlib DOES provide a real stdlib.h; the
 * problem is a genuine name collision, not a missing header. newlib's
 * stdlib.h declares non-standard 3-arg itoa(int, char*, int)/
 * utoa(unsigned, char*, int) extensions (gated on __MISC_VISIBLE, which is
 * on by default here), which collide with BusyBox's own unrelated 1-arg
 * itoa(int)/utoa(unsigned) helpers (include/libbb.h, implemented in
 * libbb/utoa_to_buf.c) — same names, different signatures, so both
 * declarations being visible in the same translation unit is a hard
 * compile error, not just a warning.
 *
 * Renaming newlib's versions out of the way (nothing in BusyBox's source
 * calls the 3-arg forms) lets both coexist without patching BusyBox
 * itself. #include_next finds the next <stdlib.h> on the include path —
 * i.e. newlib's real one — since this file's directory
 * (user/newlib_compat) is searched first (see the Makefile's
 * NEWLIB_CFLAGS).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_STDLIB_H
#define PUREUNIX_NEWLIB_COMPAT_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#define itoa __newlib_itoa
#define utoa __newlib_utoa
#include_next <stdlib.h>
#undef itoa
#undef utoa

/* clearenv(): a real GNU/POSIX extension newlib just doesn't declare
 * anywhere (implemented for real in user/newlib_syscalls.c). */
int clearenv(void);


#ifdef __cplusplus
}
#endif
#endif
