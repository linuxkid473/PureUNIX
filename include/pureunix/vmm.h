#ifndef PUREUNIX_VMM_H
#define PUREUNIX_VMM_H

#include <pureunix/config.h>
#include <pureunix/types.h>

/* x86 page-table entry flags */
#define PAGE_PRESENT    0x001U
#define PAGE_WRITE      0x002U
#define PAGE_USER       0x004U
#define PAGE_GLOBAL     0x100U

/*
 * Virtual address space layout
 *
 * 0x00000000 – 0x00FFFFFF  kernel space (pd[0..3], identity mapped, no PAGE_USER)
 *   0x00000000 – 0x000FFFFF  reserved low 1 MiB
 *   0x00100000 – kernel_end  kernel image
 *   kernel_end – +8 MiB      kernel heap
 *
 * 0x01000000 – 0x01EFFFFF  user code / data  (pd[4..7])
 * 0x01FF0000 – 0x01FFFFFF  user stack        (pd[7], grows down)
 *
 * 0x02000000 – 0x07FFFFFF  upper kernel-only space (pd[8..31], identity)
 */
#define USER_CODE_BASE      0x01000000U
#define USER_CODE_LIMIT     0x01F00000U

#define USER_STACK_PAGES    16U
#define USER_STACK_TOP      0x02000000U
#define USER_STACK_BOTTOM   (USER_STACK_TOP - USER_STACK_PAGES * PUREUNIX_PAGE_SIZE)

/* pd indices: user virtual space occupies pd[USER_PD_START .. USER_PD_END-1] */
#define USER_PD_START   4U
#define USER_PD_END     8U

/* Per-process page directory ----------------------------------------------- */
uint32_t    vmm_create_user_directory(void);
void        vmm_free_user_directory(uint32_t pd_phys);
void        vmm_map_page_in(uint32_t pd_phys, virt_addr_t virt,
                             phys_addr_t phys, uint32_t flags);

/* Address-space switching -------------------------------------------------- */
void        vmm_switch_directory(uint32_t pd_phys);
void        vmm_switch_directory_kernel(void);
uint32_t    vmm_kernel_directory_phys(void);

/* Kernel virtual-memory allocator ------------------------------------------ */
void       *vm_alloc(size_t pages, uint32_t flags);
void        vm_free(void *ptr, size_t pages);

#endif
