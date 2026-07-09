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

#define MULTIBOOT2_TAG_MODULE 3
#define MULTIBOOT2_TAG_FRAMEBUFFER 8
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB 1

/* GRUB's `module2 /boot/foo.img foo.img` — mod_start/mod_end are physical
 * addresses of the already-loaded module; cmdline is whatever string
 * followed the path in grub.cfg (used here to identify which image a
 * module is, rather than relying on load order). */
typedef struct multiboot2_module_tag {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
} __attribute__((packed)) multiboot2_module_tag_t;

/* Fixed-size header; for framebuffer_type == RGB it is immediately followed
 * by a multiboot2_framebuffer_rgb_info_t describing the channel layout. */
typedef struct multiboot2_framebuffer_tag {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
} __attribute__((packed)) multiboot2_framebuffer_tag_t;

typedef struct multiboot2_framebuffer_rgb_info {
    uint8_t red_field_position;
    uint8_t red_mask_size;
    uint8_t green_field_position;
    uint8_t green_mask_size;
    uint8_t blue_field_position;
    uint8_t blue_mask_size;
} __attribute__((packed)) multiboot2_framebuffer_rgb_info_t;

#endif
