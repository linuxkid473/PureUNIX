#ifndef PUREUNIX_STRING_H
#define PUREUNIX_STRING_H

#include <pureunix/types.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *a, const void *b, size_t count);
size_t strlen(const char *str);
size_t strnlen(const char *str, size_t max);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *str, int ch);
char *strrchr(const char *str, int ch);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *src);
char *strtok_r(char *str, const char *delim, char **saveptr);

#endif
