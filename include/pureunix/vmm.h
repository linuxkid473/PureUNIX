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
 * 0x08000000 – 0x0BFFFFFF  per-process user window (pd[32..47], private):
 *   0x08000000 – (heap)      code / data / bss (see user/linker.ld) --
 *                            every program installed on the image today is
 *                            well under 1.5 MiB of this
 *   HEAP_VA (below)          real, incrementally-grown sbrk() heap
 *                            (SYS_SBRK, arch/i386/syscall.c) -- pages are
 *                            mapped one at a time as the break actually
 *                            grows, up to HEAP_MAX, not eagerly reserved
 *                            up front (see HEAP_VA's own comment for why)
 *   FB_SHADOW_VA (below)     SDL2 port's window-surface pixel buffer
 *                            (docs/sdl-port.md), mapped on demand by
 *                            SYS_FB_MMAP -- also real per-process pages,
 *                            not part of the ELF image or the heap
 *   USER_WINDOW_END-64KiB    fixed-size stack, growing down
 *
 * 0x0C000000 – 0xFFFFFFFF  unmapped
 *
 * pd[32..47] are the only PDEs that differ between processes: vmm_create_
 * user_directory() clones the kernel's own page_directory (so pd[0..31]
 * alias the shared kernel tables) and leaves pd[32..47] absent, to be
 * populated per-process by vmm_map_page_in(). Widened progressively as
 * real applications needed more room -- 3 MiB (pd[32] only) originally,
 * then 16 MiB for the SDL2 port's FB_SHADOW_VA (docs/sdl-port.md), now
 * 64 MiB for the Chocolate Doom port (docs/chocolate-doom-port.md): Doom's
 * own zone allocator (src/z_zone.c) mallocs up to 16-32 MiB in one shot
 * for its private memory pool, on top of SDL's own window-surface mapping,
 * neither of which fit in the previous 16 MiB total. Virtual address space
 * this size costs nothing by itself -- vmm_map_page_in() only spends a
 * real physical frame for a page some process actually touches, whether
 * via HEAP_VA's incremental growth or FB_SHADOW_VA's one-shot mapping --
 * so widening this is not the same kind of cost the eager-bss-array
 * design HEAP_VA replaced was (see that comment). vmm_free_user_
 * directory() and vmm_fork_address_space() (kernel/vmm.c) both loop over
 * every PDE in [USER_WINDOW_BASE, USER_WINDOW_END), not a fixed count.
 */
#define USER_WINDOW_BASE    0x08000000U
#define USER_WINDOW_END     0x0C000000U
#define USER_STACK_SIZE     0x10000U

/* One page reserved immediately below the stack for kernel/signal.c's
 * per-process signal trampoline (mapped read+exec, never write, at exec()
 * time — see arch/i386/signal.c) — never available to a program's own
 * PT_LOAD segments; kernel/elf.c's ELF_CODE_LIMIT is shrunk by exactly
 * this one page to guarantee no real program's code/data/bss can ever
 * extend into it. See docs/process-management.md. */
#define SIGNAL_TRAMPOLINE_VA (USER_WINDOW_END - USER_STACK_SIZE - PUREUNIX_PAGE_SIZE)

/* Real, incrementally-grown sbrk() heap (SYS_SBRK, arch/i386/syscall.c;
 * user/newlib_syscalls.c's sbrk() is now a thin wrapper around it) --
 * replaces what used to be a fixed-size static array in every newlib
 * program's own bss (NEWLIB_HEAP_SIZE), eagerly costing every single
 * program that many real physical MiB at exec() time regardless of
 * whether it ever used that much heap (kernel/elf.c's PT_LOAD loader has
 * no demand paging -- see docs/sdl-port.md's "eager bss physical
 * allocation" note, the exact lesson this generalizes). SYS_SBRK instead
 * maps one real page at a time as task_t.heap_used actually grows past
 * task_t.heap_mapped, up to HEAP_MAX -- a tiny program that barely
 * touches malloc() now costs only what it actually touches, while a
 * program that needs Chocolate Doom's ~16-32 MiB zone allocation
 * (docs/chocolate-doom-port.md) can still get it. Placed well clear of
 * any real program's ELF-loaded code/data/bss (4 MiB in) and, at its own
 * maximum extent, well clear of FB_SHADOW_VA below. */
#define HEAP_VA  (USER_WINDOW_BASE + 0x400000U)
#define HEAP_MAX 0x2000000U /* 32 MiB */

/* SDL2 port (docs/sdl-port.md): fixed VA, within the user window but well
 * clear of HEAP_VA's own maximum extent (HEAP_VA + HEAP_MAX) and of the
 * stack at the opposite end, where SYS_FB_MMAP (arch/i386/syscall.c) maps
 * a process's on-demand window-surface pixel buffer. Not part of any ELF
 * segment and not backed by the heap. */
#define FB_SHADOW_VA (HEAP_VA + HEAP_MAX)

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
