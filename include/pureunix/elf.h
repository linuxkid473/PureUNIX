#ifndef PUREUNIX_ELF_H
#define PUREUNIX_ELF_H

#include <pureunix/types.h>

int elf_exec(const char *path);
bool elf_is_valid(const uint8_t *image, size_t size);

#endif
