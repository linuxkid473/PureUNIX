# Memory Management

## Overview

PureUnix has three memory management layers:

1. **Physical Memory Manager (PMM)** — tracks which 4 KiB frames are free using a bitmap.
2. **Virtual Memory Manager (VMM)** — builds a page directory and identity-maps the first 128 MiB.
3. **Kernel Heap** — provides `kmalloc`/`kfree` using a linked-list allocator.

All three are in `kernel/`. Their public API is declared in `include/pureunix/memory.h`.

---

## Physical Memory Manager

**Source**: `kernel/pmm.c`

### Design

The PMM uses a flat bitmap: one bit per 4 KiB physical frame. A set bit (`1`) means the frame is allocated or reserved; a clear bit (`0`) means it is free.

```
Capacity:  128 MiB maximum (32768 frames)
Bitmap:    uint32_t frame_bitmap[1024]   (32768 bits = 4 KiB)
```

The bitmap starts fully set (all frames reserved). The PMM then walks the Multiboot memory map and clears bits for usable RAM regions, then re-sets bits for:

- Low memory (0x000000–0x0FFFFF, 1 MiB) — IVT, BDA, BIOS
- The kernel image (`__kernel_start` to `__kernel_end`)
- The bitmap itself
- Every GRUB boot module (`fat.img`/`root.img` — see below and `docs/boot.md`'s "Boot Modules" section), so `pmm_alloc_frame()` never hands out a frame a mounted ramdisk is still using
- The kernel heap's 8 MiB range (via `heap_reserved_range()`, computed the same way `heap_init()` derives it — see Kernel Heap below): the heap is carved out directly by linker symbols, never through this allocator, so without this reservation `pmm_alloc_frame()` would consider heap-resident memory free and could hand out a frame that a live `kmalloc()`'d buffer (or the ext2 block cache) is still using

### Multiboot Support

Both Multiboot1 and Multiboot2 are supported. The `magic` value passed by the boot loader determines which parser runs:

| Magic | Format |
|---|---|
| `0x2BADB002` | Multiboot1: `multiboot1_info_t`, mmap at `mbi->mmap_addr` |
| `0x36D76289` | Multiboot2: tag stream; type-6 tag contains the memory map, type-3 tags contain boot modules |

If neither magic is recognized, 15 MiB starting at 1 MiB is assumed free (fallback for unknown loaders).

### Boot Modules

While walking Multiboot2 tags, `parse_multiboot2()` also records every type-3 (module) tag — `(mod_start, mod_end, cmdline)` — into a small fixed-size table (`MAX_BOOT_MODULES`, currently 4), then reserves each one's physical range the same way the kernel image and heap are reserved. `cmdline` is whatever string followed the module's path in `boot/grub.cfg` (`"fat.img"`/`"root.img"`), used by `kernel_main()` to identify which image a module is. See `docs/boot.md`'s "Boot Modules" section for how these become mounted filesystems.

```c
uint32_t              pmm_module_count(void);
const boot_module_t  *pmm_module_get(uint32_t index);
uint32_t              pmm_modules_end(void);   // highest mod_end, or 0 if none
```

`pmm_modules_end()` exists specifically so `heap_reserved_range()` can start the heap past any module GRUB happened to place inside what would otherwise be the heap's default range — see Kernel Heap below.

### API

```c
void        pmm_init(uint32_t magic, uint32_t mbi_addr);
phys_addr_t pmm_alloc_frame(void);   // returns 0 on failure
void        pmm_free_frame(phys_addr_t frame);
uint32_t    pmm_total_memory_kb(void);
uint32_t    pmm_free_memory_kb(void);
```

`pmm_alloc_frame` performs a linear scan from frame 0. It sets the corresponding bitmap bit and returns the frame's physical byte address (frame index × 4096).

### Limitations

- The 128 MiB cap is hard-coded (`MAX_MEMORY_BYTES`). Machines with more RAM have only the first 128 MiB managed.
- Linear allocation scan is O(n) in the worst case.

---

## Virtual Memory Manager

**Source**: `kernel/vmm.c`

### Design

The VMM maintains one *kernel* page directory (`page_directory`, a static 4 KiB-aligned array) that identity-maps the first 128 MiB (virtual address == physical address) by pre-populating 32 page tables, plus one *private* page directory per user-mode process.

```
page_directory[1024]           static, 4 KiB aligned — the kernel directory
identity_tables[32][1024]      static, each 4 KiB aligned
```

Each `identity_tables[t][i]` entry maps virtual address `(t*1024 + i) * 4096` to the same physical address, with flags `PAGE_PRESENT | PAGE_WRITE`. After populating the tables, `vmm_init` loads `page_directory` into `CR3` and sets `CR0.PG = 1`.

**Per-process address spaces**: `vmm_create_user_directory()` allocates a fresh physical frame and `memcpy`s the entire kernel `page_directory` into it — every PDE below `USER_WINDOW_BASE`'s index (32) is copied *by reference*, so every process's directory points at the exact same kernel page tables (kernel image, heap, framebuffer, etc. are identical and simultaneously valid in every address space). Only PDE 32 — the fixed user code/data/stack window — differs per process, populated lazily by `vmm_map_page_in()`. Because `pmm_alloc_frame()` never hands out a frame inside the heap or kernel image (see PMM above), and the user window's virtual range (0x08000000+) is entirely outside the 128 MiB identity map, any physical frame the allocator returns is always identity-accessible via `(void *)frame` regardless of which directory is currently loaded in CR3 — this is what lets `vmm_fork_address_space()` and the ELF loader fill in a not-yet-active directory's pages by writing straight to the frame's physical address.

### Flags

```c
#define PAGE_PRESENT 0x001
#define PAGE_WRITE   0x002
#define PAGE_USER    0x004
```

### Address space layout

```
0x00000000 – 0x07FFFFFF  kernel space: identity mapped, shared by every process
                          (pd[0..31] alias the same page tables in every directory)
0x08000000 – 0x082FFFFF  per-process user window (pd[32], private per process)
                            0x08000000 – (stack)   code / data / bss
                            USER_WINDOW_END-64KiB   fixed-size stack, growing down
0x08300000 – 0xFFFFFFFF  unmapped
```

### API

```c
void        vmm_init(phys_addr_t identity_extra_base, uint32_t identity_extra_size);
void        vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags);          // kernel directory only
phys_addr_t vmm_get_mapping(virt_addr_t virt);                                          // kernel directory only

uint32_t    vmm_create_user_directory(void);     // returns a new directory's phys addr, or 0 on OOM
void        vmm_free_user_directory(uint32_t pd_phys);
void        vmm_map_page_in(uint32_t pd_phys, virt_addr_t virt, phys_addr_t phys, uint32_t flags);
bool        vmm_fork_address_space(uint32_t dst_pd_phys, uint32_t src_pd_phys);          // deep-copies the user window

void        vmm_switch_directory(uint32_t pd_phys);   // loads CR3
void        vmm_switch_directory_kernel(void);
uint32_t    vmm_kernel_directory_phys(void);
```

`vmm_map_page`/`vmm_map_page_in` look up the page directory entry for `virt >> 22`. If no page table is present, one is allocated via `pmm_alloc_frame`, zeroed, and installed. The page table entry is then set; `invlpg` only runs when the target directory is the one currently active in CR3 (a directory being built for a not-yet-scheduled process gets no TLB flush).

`task_yield()` (`kernel/task.c`) calls `vmm_switch_directory()` whenever the next task's `pd_phys` differs from the current one, so CR3 always reflects whichever task is running.

### Limitations

- No `vmm_unmap_page`: pages can be mapped but not unmapped (a process's entire user window is freed at once, by `vmm_free_user_directory()`, not page by page).
- No page fault handler: accessing an unmapped page causes an unhandled exception (kernel panic) — `fork()`/`exec()` always eagerly map every page up front, so this is only reachable via a genuine bug (e.g. a wild pointer) in a user program.
- The 32 static kernel page tables and the kernel page directory are fixed in the kernel `.bss` segment: they consume 32 × 4 KiB + 4 KiB = 132 KiB of kernel memory. Each additional process directory costs one more page-directory frame plus, once it has any mappings, one page-table frame for PDE 32.

---

## Kernel Heap

**Source**: `kernel/heap.c`

### Design

The heap is a contiguous 8 MiB region starting at `ALIGN_UP(__kernel_end, PAGE_SIZE)`, *or* past the last GRUB boot module if one was loaded there instead (`heap_reserved_range()` takes `max(__kernel_end, pmm_modules_end())` — see the PMM's "Boot Modules" section above). It is fully within the identity-mapped region, so no additional VMM mappings are needed. `heap_init()` calls `heap_reserved_range()` itself rather than recomputing the base, so the two can never disagree about where the heap starts.

```
HEAP_SIZE  = 8 MiB (8 * 1024 * 1024 bytes)
HEAP_MAGIC = 0xC0FFEE42
```

The allocator maintains a doubly-linked list of `heap_block_t` headers:

```c
typedef struct heap_block {
    uint32_t        magic;   // HEAP_MAGIC; used to detect corruption
    size_t          size;    // usable bytes in this block (excluding header)
    bool            free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;
```

### Allocation

`kmalloc(size)`:
1. Rounds `size` up to the next multiple of 8 (alignment guarantee).
2. Scans the list from `heap_head` for the first free block with `size >= requested`.
3. Calls `split_block` if the block is large enough to split (must leave at least `sizeof(heap_block_t) + 16` bytes in the remainder).
4. Marks the block not-free and returns a pointer to the byte immediately after the header.

`kcalloc(count, size)` calls `kmalloc(count * size)` and zeroes the result with `memset`.

### Deallocation

`kfree(ptr)`:
1. Retrieves the header by subtracting `sizeof(heap_block_t)` from `ptr`.
2. Verifies `magic == HEAP_MAGIC`. If the magic is wrong, the call silently returns (no panic, no crash).
3. Marks the block free.
4. Calls `coalesce(block)`, which merges the block with any free adjacent block on either side.

### Reallocation

`krealloc(ptr, size)`:
- If `ptr == NULL`, equivalent to `kmalloc(size)`.
- If `size == 0`, equivalent to `kfree(ptr)`, returns NULL.
- If the current block's `size >= size`, returns `ptr` unchanged.
- Otherwise, allocates a new block, copies the old data (up to the old block's `size`), frees the old block, and returns the new pointer.

### Diagnostics

```c
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);
```

Both functions walk the entire list. They are called by the `free` shell builtin.

### Limitations

- The heap cannot grow: if the 8 MiB region is exhausted, `kmalloc` returns `NULL`.
- The allocator does not panic on heap exhaustion; callers must check for NULL.
- `kfree` does not panic on a bad magic value, making silent heap corruption possible.

---

## Memory Diagram

```
Physical Address Space (identity mapped in every process's directory)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000_0000
    │  1 MiB low memory (PMM reserved)
    │  IVT, BDA, VGA ROM, BIOS
0x0010_0000  (__kernel_start)
    │  Kernel .text / .rodata / .data / .bss
    │  Includes 32 KiB boot stack
(__kernel_end)
    │  [ALIGN_UP to 4 KiB]
    │
    │  Kernel heap
    │  8 MiB linked-list allocator (kmalloc/kfree)
    │  reserved in the PMM bitmap — pmm_alloc_frame() never hands this out
    │
0x000B_8000  VGA framebuffer  (within mapped region)
    │
0x0800_0000  End of the 128 MiB identity map / PMM-managed range
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Virtual Address Space (per-process beyond this point)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0800_0000  User ELF load start (USER_WINDOW_BASE, pd[32], private per process)
0x0830_0000  User window end (USER_WINDOW_END) — last 64 KiB is the stack
0x0830_0000 – 0xFFFF_FFFF  Unmapped (accessing causes page fault / kernel panic)
```

Note the user window's *virtual* addresses (0x08000000+) are private per process, but the *physical* frames backing them are ordinary frames from the same 128 MiB PMM pool as everything else — there is no dedicated physical range for user pages, unlike the pre-per-process design where user code shared physical memory with the kernel's identity map 1:1.
