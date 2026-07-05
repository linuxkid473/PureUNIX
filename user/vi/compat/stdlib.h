#ifndef VI_COMPAT_STDLIB_H
#define VI_COMPAT_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);

int atoi(const char *s);
long strtol(const char *s, char **endptr, int base);

/* No environment on PureUNIX: always returns NULL. Every upstream call
 * site (EXINIT, HOME, TAGPATH, LINES, COLUMNS) already guards with
 * `if (getenv(...))`, so this degrades to built-in defaults everywhere. */
char *getenv(const char *name);

void exit(int code) __attribute__((noreturn));

#endif
