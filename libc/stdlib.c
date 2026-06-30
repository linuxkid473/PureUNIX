#include <pureunix/ctype.h>
#include <pureunix/stdlib.h>
#include <pureunix/string.h>

int atoi(const char *str)
{
    return (int)strtol(str, NULL, 10);
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    long sign = 1;
    long value = 0;

    while (isspace(*s)) {
        ++s;
    }
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }

    while (*s) {
        int digit;
        if (isdigit(*s)) {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'z') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'Z') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        ++s;
    }

    if (endptr) {
        *endptr = (char *)s;
    }
    return value * sign;
}

void reverse(char *str)
{
    size_t len = strlen(str);
    for (size_t i = 0; i < len / 2; ++i) {
        char tmp = str[i];
        str[i] = str[len - i - 1];
        str[len - i - 1] = tmp;
    }
}
