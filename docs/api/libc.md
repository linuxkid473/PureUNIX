# API Reference: libc

These functions are available to all kernel code via the freestanding libc in `libc/`. They are **not** available to user programs; user programs have access only to `libpure`.

---

## stdio

**Header**: `<pureunix/stdio.h>`

```c
void putchar(int ch);
```
Writes one character to the VGA console via `vga_putc`.

```c
void puts(const char *str);
```
Writes `str` followed by `\n` to the VGA console.

```c
int printf(const char *fmt, ...);
```
Formatted output to the VGA console. See format specifiers below.

```c
int vprintf(const char *fmt, va_list args);
```
Formatted output with a pre-built va_list.

```c
int snprintf(char *buf, size_t size, const char *fmt, ...);
```
Formatted output to `buf`, writing at most `size-1` characters plus a null terminator. Returns the number of characters written (excluding null terminator).

```c
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
```
`snprintf` with a pre-built va_list.

### Supported Format Specifiers

| Specifier | Type | Description |
|---|---|---|
| `%c` | `int` | Single character |
| `%s` | `const char *` | String |
| `%d` / `%i` | `int` | Signed decimal |
| `%u` | `unsigned int` | Unsigned decimal |
| `%x` | `unsigned int` | Lowercase hexadecimal |
| `%X` | `unsigned int` | Uppercase hexadecimal |
| `%p` | `void *` | Pointer as `0x` + hex |
| `%%` | — | Literal `%` |

Width, precision, padding (`-`, `0`, `*`), length modifiers (`l`, `ll`, `h`), and floating-point (`%f`, `%e`, `%g`) are **not** implemented.

---

## stdlib

**Header**: `<pureunix/stdlib.h>`

```c
int atoi(const char *str);
```
Converts a decimal ASCII string to `int`. Stops at the first non-digit character.

```c
long strtol(const char *nptr, char **endptr, int base);
```
Converts a string to `long` in the given base (2–36). Writes the address of the first unconverted character to `*endptr` if `endptr` is not NULL.

```c
void reverse(char *str);
```
Reverses a null-terminated string in place. Used internally by `itoa`.

Note: `kmalloc`, `kfree`, `kcalloc`, `krealloc` are declared in `<pureunix/memory.h>`, not here.

---

## string

**Header**: `<pureunix/string.h>`

```c
void *memset(void *dest, int value, size_t count);
```
Fills `count` bytes of `dest` with the low byte of `value`.

```c
void *memcpy(void *dest, const void *src, size_t count);
```
Copies `count` bytes from `src` to `dest`. Behavior is undefined if regions overlap; use `memmove` in that case.

```c
void *memmove(void *dest, const void *src, size_t count);
```
Like `memcpy` but handles overlapping regions.

```c
int memcmp(const void *a, const void *b, size_t count);
```
Compares `count` bytes. Returns negative, zero, or positive.

```c
size_t strlen(const char *str);
```
Returns the number of characters before the null terminator.

```c
size_t strnlen(const char *str, size_t max);
```
Like `strlen` but stops at `max` characters.

```c
char *strcpy(char *dest, const char *src);
```
Copies `src` including null terminator to `dest`. Returns `dest`.

```c
char *strncpy(char *dest, const char *src, size_t n);
```
Copies up to `n` bytes. Pads with null bytes if `src` is shorter than `n`.

```c
char *strcat(char *dest, const char *src);
```
Appends `src` to the end of `dest`. Returns `dest`.

```c
int strcmp(const char *a, const char *b);
```
Lexicographic comparison. Returns negative, zero, or positive.

```c
int strncmp(const char *a, const char *b, size_t n);
```
Like `strcmp` but compares at most `n` characters.

```c
char *strchr(const char *str, int ch);
```
Returns a pointer to the first occurrence of `ch` in `str`, or NULL.

```c
char *strrchr(const char *str, int ch);
```
Returns a pointer to the last occurrence of `ch` in `str`, or NULL.

```c
char *strstr(const char *haystack, const char *needle);
```
Returns a pointer to the first occurrence of `needle` in `haystack`, or NULL.

```c
char *strdup(const char *src);
```
Allocates a copy of `src` via `kmalloc`. Caller must free with `kfree`.

```c
char *strtok_r(char *str, const char *delim, char **saveptr);
```
Reentrant tokenizer. On the first call, pass `str` and a pointer to a `char *` save variable. On subsequent calls for the same string, pass `NULL` as `str`. Returns the next token or NULL when exhausted. Modifies `str` in place.

---

## ctype

**Header**: `<pureunix/ctype.h>`

```c
int isdigit(int c);    // '0'–'9'
int isalpha(int c);    // 'a'–'z', 'A'–'Z'
int isalnum(int c);    // isdigit || isalpha
int isspace(int c);    // ' ', '\t', '\n', '\r', '\f', '\v'
int isupper(int c);    // 'A'–'Z'
int islower(int c);    // 'a'–'z'
int toupper(int c);    // 'a'–'z' → 'A'–'Z'; others unchanged
int tolower(int c);    // 'A'–'Z' → 'a'–'z'; others unchanged
```

All functions accept and return `int` as per C convention. No locale support.

---

## Kernel Panic

**Header**: `<pureunix/panic.h>`

```c
void panic(const char *fmt, ...) __attribute__((noreturn));
```

Disables interrupts, prints a white-on-red error message to VGA and serial using `printf`-compatible formatting, and halts indefinitely. Never returns. Used for unrecoverable kernel errors.
