#include <pureunix/kernel.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

/* Raised from 8 MiB for the Qt 6 port (docs/qt-port.md, Phase 5):
 * fs/ext2/file.c's ext2_read_file() kcalloc()s a real statically-linked
 * program's *entire* file content into one contiguous kernel-heap
 * allocation before exec() ever runs it (this kernel's "whole file in
 * memory" read model, see include/pureunix/task.h's open_file_t comment)
 * — a real Qt Gui test program (user/qtguitest.cpp, ~13-16 MB, most of it
 * real bundled FreeType/HarfBuzz/libpng code) is bigger than the whole
 * previous heap, so `kcalloc failed for N bytes` at open()/exec() time
 * was a real, hard ceiling, not something an ext2 image resize (unlike
 * the earlier docs/qt-port.md Phase 4 mkext2.py finding) could ever fix
 * — this is kernel physical memory, unrelated to disk image capacity.
 * 24 MiB leaves real headroom for QtWidgets test binaries later in this
 * same port while still comfortably fitting a 128 MiB QEMU boot (~33 MiB
 * was free at the post-boot checkpoint with the old 8 MiB heap already
 * reserved — see that same doc section for the exact numbers this was
 * sized against). */
#define HEAP_SIZE (24U * 1024U * 1024U)
#define HEAP_MAGIC 0xC0FFEE42U

typedef struct heap_block {
    uint32_t magic;
    size_t size;
    bool free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_head;
static uint8_t *heap_start;
static uint8_t *heap_end;
/* Cumulative count of kmalloc() calls that returned NULL because nothing in
 * the free list was big enough -- see heap_dump()/kernel/main.c's boot
 * checkpoints. A nonzero count here, even from a request that later
 * succeeded (e.g. after something else kfree()'d), is a sign the heap ran
 * closer to its limit than intended. */
static uint32_t alloc_failures;

static size_t align8(size_t value)
{
    return (value + 7) & ~((size_t)7);
}

/* Computable before heap_init() runs (pmm_init() calls this to keep
 * pmm_alloc_frame() from ever handing out a physical frame that overlaps
 * the live heap — see the pmm_init() call site). Starts past whichever is
 * higher of the kernel image or the last GRUB module (root.img/fat.img,
 * see kernel_main): pmm_init() parses modules before calling this, so
 * pmm_modules_end() is already populated by the time this runs. Without
 * this, a module GRUB happened to place inside [__kernel_end, +8MiB) would
 * get its opening bytes stomped by heap_head itself. */
void heap_reserved_range(phys_addr_t *base, uint32_t *size)
{
    uint32_t after_kernel = ALIGN_UP((uint32_t)&__kernel_end, PUREUNIX_PAGE_SIZE);
    uint32_t after_modules = ALIGN_UP(pmm_modules_end(), PUREUNIX_PAGE_SIZE);
    *base = after_modules > after_kernel ? after_modules : after_kernel;
    *size = HEAP_SIZE;
}

void heap_init(void)
{
    phys_addr_t base;
    uint32_t size;
    heap_reserved_range(&base, &size);
    heap_start = (uint8_t *)base;
    heap_end = heap_start + size;
    heap_head = (heap_block_t *)heap_start;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->free = true;
    heap_head->next = NULL;
    heap_head->prev = NULL;
}

static void split_block(heap_block_t *block, size_t size)
{
    if (block->size < size + sizeof(heap_block_t) + 16) {
        return;
    }
    heap_block_t *next = (heap_block_t *)((uint8_t *)(block + 1) + size);
    next->magic = HEAP_MAGIC;
    next->size = block->size - size - sizeof(heap_block_t);
    next->free = true;
    next->next = block->next;
    next->prev = block;
    if (next->next) {
        next->next->prev = next;
    }
    block->next = next;
    block->size = size;
}

void *kmalloc(size_t size)
{
    if (!size) {
        return NULL;
    }
    size = align8(size);
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->magic == HEAP_MAGIC && b->free && b->size >= size) {
            split_block(b, size);
            b->free = false;
            return b + 1;
        }
    }
    alloc_failures++;
    return NULL;
}

void *kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void coalesce(heap_block_t *block)
{
    if (block->next && block->next->free) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    if (block->prev && block->prev->free) {
        coalesce(block->prev);
    }
}

void kfree(void *ptr)
{
    if (!ptr) {
        return;
    }
    heap_block_t *block = ((heap_block_t *)ptr) - 1;
    if (block->magic != HEAP_MAGIC) {
        return;
    }
    block->free = true;
    coalesce(block);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr) {
        return kmalloc(size);
    }
    if (!size) {
        kfree(ptr);
        return NULL;
    }
    heap_block_t *block = ((heap_block_t *)ptr) - 1;
    if (block->size >= size) {
        return ptr;
    }
    void *next = kmalloc(size);
    if (!next) {
        return NULL;
    }
    memcpy(next, ptr, block->size);
    kfree(ptr);
    return next;
}

size_t heap_free_bytes(void)
{
    size_t total = 0;
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->free) {
            total += b->size;
        }
    }
    return total;
}

size_t heap_used_bytes(void)
{
    return (size_t)(heap_end - heap_start) - heap_free_bytes();
}

size_t heap_largest_free_block(void)
{
    size_t largest = 0;
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size > largest) {
            largest = b->size;
        }
    }
    return largest;
}

uint32_t heap_alloc_failures(void)
{
    return alloc_failures;
}

bool heap_check_integrity(void)
{
    heap_block_t *prev = NULL;
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) {
            printf("heap: CORRUPT: block %p has bad magic %x (expected %x)\n", (void *)b,
                   (unsigned)b->magic, (unsigned)HEAP_MAGIC);
            return false;
        }
        if ((uint8_t *)b < heap_start || (uint8_t *)(b + 1) + b->size > heap_end) {
            printf("heap: CORRUPT: block %p size=%u extends outside heap range [%p, %p)\n",
                   (void *)b, (unsigned)b->size, (void *)heap_start, (void *)heap_end);
            return false;
        }
        if (b->prev != prev) {
            printf("heap: CORRUPT: block %p prev=%p, walk expected %p\n", (void *)b,
                   (void *)b->prev, (void *)prev);
            return false;
        }
        /* This allocator never leaves a gap between adjacent blocks (see
         * split_block()/coalesce() above), so block N's declared size must
         * land exactly on block N+1's header -- if it doesn't, either a
         * write ran past the end of this block's payload into the next
         * block's header, or `size` itself got clobbered. */
        if (b->next && (uint8_t *)b->next != (uint8_t *)(b + 1) + b->size) {
            printf("heap: CORRUPT: block %p size=%u -- next=%p, expected %p\n", (void *)b,
                   (unsigned)b->size, (void *)b->next, (void *)((uint8_t *)(b + 1) + b->size));
            return false;
        }
        prev = b;
    }
    return true;
}

void heap_dump(const char *label)
{
    bool ok = heap_check_integrity();
    printf("heap[%s]: start=%p end=%p total=%u used=%u free=%u largest_free=%u "
           "alloc_failures=%u integrity=%s\n",
           label, (void *)heap_start, (void *)heap_end, (unsigned)(heap_end - heap_start),
           (unsigned)heap_used_bytes(), (unsigned)heap_free_bytes(),
           (unsigned)heap_largest_free_block(), (unsigned)alloc_failures, ok ? "OK" : "CORRUPT");
}
