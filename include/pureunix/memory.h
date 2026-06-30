#ifndef PUREUNIX_MEMORY_H
#define PUREUNIX_MEMORY_H

#include <pureunix/config.h>
#include <pureunix/types.h>

void pmm_init(uint32_t magic, uint32_t mbi_addr);
phys_addr_t pmm_alloc_frame(void);
void pmm_free_frame(phys_addr_t frame);
uint32_t pmm_total_memory_kb(void);
uint32_t pmm_free_memory_kb(void);

void vmm_init(void);
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags);
phys_addr_t vmm_get_mapping(virt_addr_t virt);

void heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);

#endif
