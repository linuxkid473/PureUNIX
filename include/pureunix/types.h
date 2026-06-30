#ifndef PUREUNIX_TYPES_H
#define PUREUNIX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t ssize_t;
typedef uint32_t phys_addr_t;
typedef uint32_t virt_addr_t;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ALIGN_UP(value, align) (((value) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(value, align) ((value) & ~((align) - 1))

#endif
