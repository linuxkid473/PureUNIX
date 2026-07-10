#ifndef PUREUNIX_VMM_H
#define PUREUNIX_VMM_H

#include <pureunix/config.h>
#include <pureunix/types.h>

/* x86 page-table entry flags */
#define PAGE_PRESENT    0x001U
#define PAGE_WRITE      0x002U
#define PAGE_USER       0x004U
#define PAGE_PWT        0x008U
#define PAGE_PCD        0x010U
#define PAGE_GLOBAL     0x100U
/* PAT bit (bit 7) of a 4KiB PTE. Combined with PCD/PWT (bits 4/3), it picks
 * one of the 8 PAT-MSR entries as (PAT:PCD:PWT):
 *
 *   PAT=0,PCD=1,PWT=1 -> entry 3 -> UC (strong, MTRR-independent) — the
 *                        hardware reset default, never reprogrammed by this
 *                        kernel. vmm_map_mmio_uc() below selects this for
 *                        every PCI device register mapping (xHCI, e1000,
 *                        ...): real hardware needs those writes to reach the
 *                        device immediately and in issue order, which only
 *                        true UC guarantees.
 *   PAT=1,PCD=0,PWT=0 -> entry 4 -> Write-Combining, reprogrammed from its
 *                        Write-Back reset default by enable_pat_write_
 *                        combining() in vmm.c. vmm_map_framebuffer_wc() below
 *                        selects this, and only this, for the linear
 *                        framebuffer's own bytes — see the comment above
 *                        enable_pat_write_combining() for why WC is not
 *                        optional for framebuffer performance on real
 *                        hardware, and the comment above vmm_map_mmio_uc()
 *                        for why it must never leak onto a PCI BAR.
 *
 * Entries 0-2 and 5-7 are never touched and keep their reset semantics. */
#define PAGE_PAT        0x080U

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

/* One page reserved immediately below the stack for kernel/signal.c's
 * per-process signal trampoline (mapped read+exec, never write, at exec()
 * time — see arch/i386/signal.c) — never available to a program's own
 * PT_LOAD segments; kernel/elf.c's ELF_CODE_LIMIT is shrunk by exactly
 * this one page to guarantee no real program's code/data/bss can ever
 * extend into it. See docs/process-management.md. */
#define SIGNAL_TRAMPOLINE_VA (USER_WINDOW_END - USER_STACK_SIZE - PUREUNIX_PAGE_SIZE)

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
