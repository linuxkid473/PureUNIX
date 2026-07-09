#ifndef PUREUNIX_MEMORY_H
#define PUREUNIX_MEMORY_H

#include <pureunix/config.h>
#include <pureunix/types.h>

void pmm_init(uint32_t magic, uint32_t mbi_addr);
phys_addr_t pmm_alloc_frame(void);
void pmm_free_frame(phys_addr_t frame);
/* Finds and reserves `count` physically contiguous free frames in a single
 * bitmap scan (see kernel/pmm.c) -- use this instead of calling
 * pmm_alloc_frame() in a loop for anything beyond a handful of frames; that
 * approach is O(count * total_frames) and was observed to stall for
 * seconds building a ~1000-frame run (see fb_enable_shadow() in
 * drivers/framebuffer.c). Returns 0 (nothing reserved) if no run that long
 * exists. */
phys_addr_t pmm_alloc_contiguous(uint32_t count);
void pmm_free_contiguous(phys_addr_t base, uint32_t count);
uint32_t pmm_total_memory_kb(void);
uint32_t pmm_free_memory_kb(void);

/* GRUB modules (boot/grub.cfg's `module2` lines), discovered by pmm_init()
 * while it walks the Multiboot2 info — see kernel/pmm.c. cmdline is the
 * string that followed the module's path in grub.cfg, used to identify
 * which image a module is (e.g. "root.img", "fat.img"). */
typedef struct {
    uint32_t start;
    uint32_t end;
    char cmdline[64];
} boot_module_t;

uint32_t pmm_module_count(void);
const boot_module_t *pmm_module_get(uint32_t index);
/* Highest mod_end among all recorded modules (0 if none) — heap_init() uses
 * this to make sure the kernel heap never starts inside a module's memory. */
uint32_t pmm_modules_end(void);

void vmm_init(phys_addr_t identity_extra_base, uint32_t identity_extra_size);
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags);
phys_addr_t vmm_get_mapping(virt_addr_t virt);

/* Identity-maps [base, base+size) 1:1, page by page, as Write-Combining --
 * for the linear framebuffer's own bytes only (see PAGE_PAT in vmm.h). Only
 * pages that actually fall inside [base, base+size) are touched; unlike the
 * old map_extra_region() this replaced, it never marks a whole 4MiB-aligned
 * PD chunk present+WC as a side effect, so a PCI BAR that happens to share
 * that chunk (this is what broke the xHCI keyboard on real hardware -- see
 * kernel/vmm.c) is left untouched for vmm_map_mmio_uc() to claim normally.
 * Early-boot only: called once from vmm_init(), before pmm_alloc_frame() is
 * available, so it hands out pages from vmm.c's static fb_tables reserve
 * rather than the general frame allocator. */
void vmm_map_framebuffer_wc(phys_addr_t base, uint32_t size);

/* Identity-maps one page as strong Uncacheable device memory -- for PCI MMIO
 * BAR registers (xHCI, e1000, ...). Forces PAGE_PCD|PAGE_PWT (PAGE_PAT left
 * clear) regardless of what's in `flags`, so the mapping is UC at the paging
 * level no matter what a coincidentally-neighboring identity-mapped region
 * (e.g. the framebuffer) set for an adjacent page. Safe to call any time
 * after vmm_init() (thin wrapper over vmm_map_page()); `flags` may still
 * carry PAGE_WRITE/PAGE_USER. */
void vmm_map_mmio_uc(virt_addr_t virt, phys_addr_t phys, uint32_t flags);

/* Physical range heap_init() will carve out, computable before it runs —
 * used by pmm_init() to keep the frame allocator from ever handing out a
 * frame that overlaps the live heap. */
void heap_reserved_range(phys_addr_t *base, uint32_t *size);
void heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);
size_t heap_largest_free_block(void);
uint32_t heap_alloc_failures(void);

/* Walks the free-list validating every block's magic and that prev/next/size
 * are mutually consistent (see kernel/heap.c) -- catches heap metadata
 * corruption (an out-of-bounds write into a neighboring allocation) at the
 * point it's checked, rather than letting it resurface later as an
 * unrelated-looking kmalloc() failure or crash. Returns false and prints the
 * first inconsistency found. */
bool heap_check_integrity(void);

/* Prints one line: heap bounds, used/free/largest-free-block byte counts,
 * cumulative allocation failures, and a heap_check_integrity() verdict.
 * Intended to be sprinkled at boot checkpoints (see kernel/main.c) so a
 * "kcalloc failed" report can be traced back to exactly which subsystem's
 * allocations actually consumed the heap. */
void heap_dump(const char *label);

#endif
