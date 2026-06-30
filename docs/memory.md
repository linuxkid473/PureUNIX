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

### Multiboot Support

Both Multiboot1 and Multiboot2 are supported. The `magic` value passed by the boot loader determines which parser runs:

| Magic | Format |
|---|---|
| `0x2BADB002` | Multiboot1: `multiboot1_info_t`, mmap at `mbi->mmap_addr` |
| `0x36D76289` | Multiboot2: tag stream; type-6 tag contains the memory map |

If neither magic is recognized, 15 MiB starting at 1 MiB is assumed free (fallback for unknown loaders).

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

The VMM creates a single page directory covering the entire 4 GiB virtual address space. It identity-maps the first 128 MiB (virtual address == physical address) by pre-populating 32 page tables.

```
page_directory[1024]           static, 4 KiB aligned
identity_tables[32][1024]      static, each 4 KiB aligned
```

Each `identity_tables[t][i]` entry maps virtual address `(t*1024 + i) * 4096` to the same physical address, with flags `PAGE_PRESENT | PAGE_WRITE`.

After populating the tables, `vmm_init` loads `page_directory` into `CR3` and sets `CR0.PG = 1`.

### Flags

```c
#define PAGE_PRESENT 0x001
#define PAGE_WRITE   0x002
#define PAGE_USER    0x004
```

### API

```c
void        vmm_init(void);
void        vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags);
phys_addr_t vmm_get_mapping(virt_addr_t virt);  // returns 0 if not mapped
```

`vmm_map_page` looks up the page directory entry for `virt >> 22`. If no page table is present, it allocates one via `pmm_alloc_frame`, initializes it to zero, and installs it. It then sets the page table entry and executes `invlpg` to flush the TLB for that address.

### Limitations

- No `vmm_unmap_page`: pages can be mapped but not unmapped.
- No page fault handler: accessing an unmapped page causes an unhandled exception (kernel panic).
- All kernel and user code runs in the same page directory. There is no per-process address space.
- The 32 static page tables and the page directory are fixed in the kernel `.bss` segment: they consume 32 × 4 KiB + 4 KiB = 132 KiB of kernel memory.

---

## Kernel Heap

**Source**: `kernel/heap.c`

### Design

The heap is a contiguous 8 MiB region starting at `ALIGN_UP(__kernel_end, PAGE_SIZE)`. It is fully within the identity-mapped region, so no additional VMM mappings are needed.

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
Physical Address Space (identity mapped)
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
    │  8 MiB linked-list allocator
    │  (kmalloc/kfree)
    │
0x000B_8000  VGA framebuffer  (within mapped region)
    │
0x0040_0000  User ELF load start (validated in elf_exec)
0x0070_0000  User ELF load end
    │
0x0800_0000  End of identity-mapped region (128 MiB)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0800_0000 – 0xFFFF_FFFF  Unmapped (accessing causes page fault / kernel panic)
```
