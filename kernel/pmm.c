#include <pureunix/kernel.h>
#include <pureunix/memory.h>
#include <pureunix/multiboot.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define MAX_MEMORY_BYTES (128U * 1024U * 1024U)
#define FRAME_SIZE PUREUNIX_PAGE_SIZE
#define MAX_FRAMES (MAX_MEMORY_BYTES / FRAME_SIZE)
#define BITMAP_WORDS (MAX_FRAMES / 32)

static uint32_t frame_bitmap[BITMAP_WORDS];
static uint32_t total_frames;
static uint32_t free_frames;

static void frame_set(uint32_t frame)
{
    if (frame >= MAX_FRAMES) {
        return;
    }
    uint32_t mask = 1U << (frame % 32);
    if (!(frame_bitmap[frame / 32] & mask)) {
        frame_bitmap[frame / 32] |= mask;
        if (free_frames) {
            free_frames--;
        }
    }
}

static void frame_clear(uint32_t frame)
{
    if (frame >= MAX_FRAMES) {
        return;
    }
    uint32_t mask = 1U << (frame % 32);
    if (frame_bitmap[frame / 32] & mask) {
        frame_bitmap[frame / 32] &= ~mask;
        free_frames++;
    }
}

static bool frame_test(uint32_t frame)
{
    return frame_bitmap[frame / 32] & (1U << (frame % 32));
}

static void mark_region_free(uint64_t base, uint64_t length)
{
    uint64_t start = ALIGN_UP((uint32_t)base, FRAME_SIZE);
    uint64_t end = ALIGN_DOWN((uint32_t)(base + length), FRAME_SIZE);
    if (end > MAX_MEMORY_BYTES) {
        end = MAX_MEMORY_BYTES;
    }
    for (uint64_t addr = start; addr + FRAME_SIZE <= end; addr += FRAME_SIZE) {
        frame_clear((uint32_t)(addr / FRAME_SIZE));
    }
}

static void reserve_region(uint32_t base, uint32_t length)
{
    uint32_t start = ALIGN_DOWN(base, FRAME_SIZE);
    uint32_t end = ALIGN_UP(base + length, FRAME_SIZE);
    if (end > MAX_MEMORY_BYTES) {
        end = MAX_MEMORY_BYTES;
    }
    for (uint32_t addr = start; addr < end; addr += FRAME_SIZE) {
        frame_set(addr / FRAME_SIZE);
    }
}

static void parse_multiboot2(uint32_t mbi_addr)
{
    uint32_t total_size = *(uint32_t *)mbi_addr;
    uint8_t *cursor = (uint8_t *)(mbi_addr + 8);
    uint8_t *end = (uint8_t *)(mbi_addr + total_size);

    while (cursor < end) {
        multiboot2_tag_t *tag = (multiboot2_tag_t *)cursor;
        if (tag->type == 0) {
            break;
        }
        if (tag->type == 6) {
            multiboot2_mmap_tag_t *mmap = (multiboot2_mmap_tag_t *)tag;
            uint8_t *entry = (uint8_t *)mmap->entries;
            uint8_t *mmap_end = (uint8_t *)tag + tag->size;
            while (entry < mmap_end) {
                multiboot2_mmap_entry_t *e = (multiboot2_mmap_entry_t *)entry;
                if (e->type == 1) {
                    mark_region_free(e->base_addr, e->length);
                }
                entry += mmap->entry_size;
            }
        }
        cursor += ALIGN_UP(tag->size, 8);
    }
}

static void parse_multiboot1(uint32_t mbi_addr)
{
    multiboot1_info_t *mbi = (multiboot1_info_t *)mbi_addr;
    if (mbi->flags & (1 << 6)) {
        uint32_t cursor = mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;
        while (cursor < end) {
            multiboot1_mmap_t *entry = (multiboot1_mmap_t *)cursor;
            if (entry->type == 1) {
                mark_region_free(entry->base_addr, entry->length);
            }
            cursor += entry->size + sizeof(entry->size);
        }
    } else if (mbi->flags & 1) {
        mark_region_free(0x100000, (uint64_t)mbi->mem_upper * 1024);
    }
}

void pmm_init(uint32_t magic, uint32_t mbi_addr)
{
    total_frames = MAX_FRAMES;
    free_frames = 0;
    for (size_t i = 0; i < BITMAP_WORDS; ++i) {
        frame_bitmap[i] = 0xFFFFFFFFU;
    }

    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        parse_multiboot2(mbi_addr);
    } else if (magic == MULTIBOOT1_BOOTLOADER_MAGIC) {
        parse_multiboot1(mbi_addr);
    } else {
        mark_region_free(0x100000, 15U * 1024U * 1024U);
    }

    reserve_region(0, 0x100000);
    reserve_region((uint32_t)&__kernel_start, (uint32_t)(&__kernel_end - &__kernel_start));
    reserve_region((uint32_t)frame_bitmap, sizeof(frame_bitmap));

    /* The kernel heap lives just past the kernel image but is carved out
     * directly by heap_init() via linker symbols, never through this
     * allocator — so without this, pmm_alloc_frame() would think those
     * frames are free and could hand one out while heap-resident data
     * (e.g. a kmalloc'd buffer, or the ext2 block cache) is still live in
     * it, corrupting whichever it overlaps. */
    phys_addr_t heap_base;
    uint32_t heap_size;
    heap_reserved_range(&heap_base, &heap_size);
    reserve_region(heap_base, heap_size);

    printf("PMM: %u KiB total, %u KiB free\n", pmm_total_memory_kb(), pmm_free_memory_kb());
}

phys_addr_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < total_frames; ++i) {
        if (!frame_test(i)) {
            frame_set(i);
            return i * FRAME_SIZE;
        }
    }
    return 0;
}

void pmm_free_frame(phys_addr_t frame)
{
    frame_clear(frame / FRAME_SIZE);
}

uint32_t pmm_total_memory_kb(void)
{
    return (total_frames * FRAME_SIZE) / 1024;
}

uint32_t pmm_free_memory_kb(void)
{
    return (free_frames * FRAME_SIZE) / 1024;
}
