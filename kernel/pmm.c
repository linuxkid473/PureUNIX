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

/* GRUB modules (boot/grub.cfg's `module2` lines) discovered while parsing
 * the Multiboot2 info — recorded here rather than a separate subsystem
 * since this file already owns "walk the MBI tags, reserve what must not
 * be handed out by pmm_alloc_frame()", and modules need exactly that.
 * boot_module_t itself lives in memory.h so kernel_main can read it back. */
#define MAX_BOOT_MODULES 4

static boot_module_t boot_modules[MAX_BOOT_MODULES];
static uint32_t boot_module_count;

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

/* E820/Multiboot region type numbers (both multiboot1's mmap_addr entries
 * and multiboot2's tag-6 entries share this numbering -- it's really just
 * E820 passed through). 1 is the only type mark_region_free() ever acts on;
 * everything else stays reserved (left set in the bitmap from pmm_init()'s
 * initial "assume nothing is free" fill) -- printed here purely as boot
 * diagnostics, to let the raw map be diffed between machines (see the "PMM
 * mmap" lines below) when free memory unexpectedly comes up short on real
 * hardware but not under QEMU. */
static const char *mmap_type_name(uint32_t type)
{
    switch (type) {
    case 1: return "usable";
    case 2: return "reserved";
    case 3: return "acpi-reclaimable";
    case 4: return "acpi-nvs";
    case 5: return "bad";
    default: return "unknown";
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
                /* base_addr/length are 64-bit (a real machine's map can
                 * describe memory above 4 GiB, e.g. behind the PCI hole on a
                 * 16 GiB HP Pavilion) but this printf() only understands
                 * 32-bit varargs -- split each into hi:lo halves explicitly
                 * rather than truncating-cast to a pointer, which would
                 * silently print a wrapped, wrong address for any region at
                 * or above 4 GiB. */
                printf("PMM mmap: base=%x_%08x len_kib=%x_%08x type=%u (%s)\n",
                       (uint32_t)(e->base_addr >> 32), (uint32_t)e->base_addr,
                       (uint32_t)((e->length / 1024) >> 32), (uint32_t)(e->length / 1024),
                       e->type, mmap_type_name(e->type));
                if (e->type == 1) {
                    mark_region_free(e->base_addr, e->length);
                }
                entry += mmap->entry_size;
            }
        } else if (tag->type == MULTIBOOT2_TAG_MODULE) {
            multiboot2_module_tag_t *mod = (multiboot2_module_tag_t *)tag;
            if (boot_module_count < MAX_BOOT_MODULES) {
                boot_module_t *bm = &boot_modules[boot_module_count++];
                bm->start = mod->mod_start;
                bm->end = mod->mod_end;
                strncpy(bm->cmdline, mod->cmdline, sizeof(bm->cmdline) - 1);
                bm->cmdline[sizeof(bm->cmdline) - 1] = '\0';
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
            printf("PMM mmap: base=%x_%08x len_kib=%x_%08x type=%u (%s)\n",
                   (uint32_t)(entry->base_addr >> 32), (uint32_t)entry->base_addr,
                   (uint32_t)((entry->length / 1024) >> 32), (uint32_t)(entry->length / 1024),
                   entry->type, mmap_type_name(entry->type));
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
    boot_module_count = 0;
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

    /* GRUB-loaded modules (root.img/fat.img, see kernel_main's use of
     * pmm_module_get()) live in otherwise-ordinary RAM that mark_region_free()
     * above already marked available — reserve them explicitly so
     * pmm_alloc_frame() never hands one out from underneath a mounted
     * ramdisk. */
    for (uint32_t i = 0; i < boot_module_count; ++i) {
        reserve_region(boot_modules[i].start, boot_modules[i].end - boot_modules[i].start);
    }

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

/* Finds and reserves `count` physically contiguous free frames in a single
 * left-to-right scan of the bitmap, or reserves nothing and returns 0 if no
 * run that long exists. This is NOT "call pmm_alloc_frame() `count` times
 * and hope" -- that was tried for the framebuffer shadow (see
 * fb_enable_shadow() in drivers/framebuffer.c) and is pathologically slow
 * for a large `count`: pmm_alloc_frame() itself is an O(total_frames) scan
 * from frame 0 every single call, so allocating N frames one at a time costs
 * O(N * total_frames) once the free run of interest sits past a large
 * already-reserved region (exactly the real layout here: the kernel image,
 * GRUB's fat.img/root.img modules, and the kernel heap are all reserved
 * back-to-back near the bottom of the low 128MiB, so any allocator scanning
 * from frame 0 has to step over all of them, every call, to reach the
 * actual multi-megabyte free region beyond the heap) -- observed in testing
 * as a multi-second stall building a ~1000-frame run under QEMU's software
 * CPU emulation, potentially much worse for a larger request. This does the
 * same "track the current run, reset on any used bit" logic but as one
 * O(total_frames) pass regardless of `count`, then commits (frame_set()s)
 * the run only once it's confirmed long enough, so a failed search never
 * leaves a partial reservation to unwind. */
phys_addr_t pmm_alloc_contiguous(uint32_t count)
{
    if (count == 0 || count > total_frames) {
        return 0;
    }
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    for (uint32_t i = 0; i < total_frames; ++i) {
        if (frame_test(i)) {
            run_len = 0;
            continue;
        }
        if (run_len == 0) {
            run_start = i;
        }
        if (++run_len == count) {
            for (uint32_t f = run_start; f < run_start + count; ++f) {
                frame_set(f);
            }
            return (phys_addr_t)run_start * FRAME_SIZE;
        }
    }
    return 0;
}

void pmm_free_contiguous(phys_addr_t base, uint32_t count)
{
    uint32_t start = base / FRAME_SIZE;
    for (uint32_t f = start; f < start + count; ++f) {
        frame_clear(f);
    }
}

uint32_t pmm_total_memory_kb(void)
{
    return (total_frames * FRAME_SIZE) / 1024;
}

uint32_t pmm_free_memory_kb(void)
{
    return (free_frames * FRAME_SIZE) / 1024;
}

uint32_t pmm_module_count(void)
{
    return boot_module_count;
}

const boot_module_t *pmm_module_get(uint32_t index)
{
    if (index >= boot_module_count) {
        return NULL;
    }
    return &boot_modules[index];
}

uint32_t pmm_modules_end(void)
{
    uint32_t end = 0;
    for (uint32_t i = 0; i < boot_module_count; ++i) {
        if (boot_modules[i].end > end) {
            end = boot_modules[i].end;
        }
    }
    return end;
}
