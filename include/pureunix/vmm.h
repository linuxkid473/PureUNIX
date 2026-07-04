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
 * 0x00000000 – 0x07FFFFFF  kernel space: identity mapped 1:1 (low 1 MiB,
 *                          kernel image, kernel heap, MMIO windows such as
 *                          the linear framebuffer). Shared by every
 *                          process: every per-process page directory below
 *                          points at these exact same page tables (pd[0..31]
 *                          are copied by reference, never duplicated).
 *
 * 0x08000000 – 0x082FFFFF  per-process user window (pd[32], private):
 *   0x08000000 – (stack)     code / data / bss (see user/linker.ld)
 *   USER_WINDOW_END-64KiB    fixed-size stack, growing down
 *
 * 0x08300000 – 0xFFFFFFFF  unmapped
 *
 * pd[32] is the only PDE that differs between processes: vmm_create_user_
 * directory() clones the kernel's own page_directory (so pd[0..31] alias
 * the shared kernel tables) and leaves pd[32] absent, to be populated
 * per-process by vmm_map_page_in().
 */
#define USER_WINDOW_BASE    0x08000000U
#define USER_WINDOW_END     0x08300000U
#define USER_STACK_SIZE     0x10000U

/* Per-process page directory ----------------------------------------------- */
uint32_t    vmm_create_user_directory(void);
void        vmm_free_user_directory(uint32_t pd_phys);
void        vmm_map_page_in(uint32_t pd_phys, virt_addr_t virt,
                             phys_addr_t phys, uint32_t flags);
/* Deep-copies the user window (code/data/stack) from src_pd into dst_pd,
 * allocating a fresh physical frame for every page present in src_pd —
 * used by fork() to give the child its own private copy of the parent's
 * address space. Returns false (leaving dst_pd partially populated) on
 * allocation failure. */
bool        vmm_fork_address_space(uint32_t dst_pd_phys, uint32_t src_pd_phys);

/* Address-space switching -------------------------------------------------- */
void        vmm_switch_directory(uint32_t pd_phys);
void        vmm_switch_directory_kernel(void);
uint32_t    vmm_kernel_directory_phys(void);

/* Kernel virtual-memory allocator ------------------------------------------ */
void       *vm_alloc(size_t pages, uint32_t flags);
void        vm_free(void *ptr, size_t pages);

#endif
