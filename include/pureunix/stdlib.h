#ifndef PUREUNIX_STDLIB_H
#define PUREUNIX_STDLIB_H

#include <pureunix/types.h>

int atoi(const char *str);
long strtol(const char *nptr, char **endptr, int base);
void reverse(char *str);

#endif
