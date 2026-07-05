#ifndef VI_COMPAT_STDIO_H
#define VI_COMPAT_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* No real streams: fprintf's first argument is only ever stderr in
 * upstream Neatvi (an unreachable error path here, since PureUNIX programs
 * are always invoked with a well-formed argv from the shell), so it's
 * accepted and ignored rather than backed by a real FILE. */
typedef int FILE;
#define stderr ((FILE *)0)
#define stdout ((FILE *)0)

int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int fprintf(FILE *stream, const char *fmt, ...);
/* printf writes to fd 1, same as term.c's own write(1,...) calls; only
 * reached from vi.c's "-s"/"-e" batch-mode paths, which the shell's `vim`/
 * `vi` invocation never selects. */
int printf(const char *fmt, ...);

#define EOF (-1)
int getchar(void);

#endif
