#ifndef PUREUNIX_MULTIBOOT_H
#define PUREUNIX_MULTIBOOT_H

#include <pureunix/types.h>

#define MULTIBOOT1_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

typedef struct multiboot1_mmap {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) multiboot1_mmap_t;

typedef struct multiboot1_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) multiboot1_info_t;

typedef struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
} multiboot2_tag_t;

typedef struct multiboot2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_mmap_entry_t;

typedef struct multiboot2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    multiboot2_mmap_entry_t entries[];
} __attribute__((packed)) multiboot2_mmap_tag_t;

#endif
