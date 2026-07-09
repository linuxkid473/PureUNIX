#ifndef PUREUNIX_MEMORY_H
#define PUREUNIX_MEMORY_H

#include <pureunix/config.h>
#include <pureunix/types.h>

void pmm_init(uint32_t magic, uint32_t mbi_addr);
phys_addr_t pmm_alloc_frame(void);
void pmm_free_frame(phys_addr_t frame);
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

#endif
