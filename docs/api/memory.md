# API Reference: Memory

**Header**: `<pureunix/memory.h>`

---

## Physical Memory Manager

### `pmm_init`

```c
void pmm_init(uint32_t magic, uint32_t mbi_addr);
```

Parses the Multiboot information structure to discover available RAM. Initializes a 32768-bit bitmap (one bit per 4 KiB frame), starting all bits set (unavailable), then marks free frames from the Multiboot memory map. Reserves the first 1 MiB and the kernel image. Must be called after `arch_init()` and before `vmm_init()`.

### `pmm_alloc_frame`

```c
phys_addr_t pmm_alloc_frame(void);
```

Allocates one 4 KiB physical frame. Performs a linear scan of the bitmap from bit 0. Returns the physical address of the allocated frame, or `0` if no free frame is available.

### `pmm_free_frame`

```c
void pmm_free_frame(phys_addr_t frame);
```

Returns a previously allocated frame to the free pool. `frame` must be 4 KiB-aligned and within the managed range.

### `pmm_total_memory_kb`

```c
uint32_t pmm_total_memory_kb(void);
```

Returns total managed memory in KiB, as reported by the Multiboot memory map. Capped at 131072 KiB (128 MiB).

### `pmm_free_memory_kb`

```c
uint32_t pmm_free_memory_kb(void);
```

Returns the number of free frames × 4 as a KiB count. O(n) in bitmap size.

---

## Virtual Memory Manager

### `vmm_init`

```c
void vmm_init(void);
```

Sets up 32 static page tables covering the first 128 MiB as an identity map (virtual == physical). Installs a page directory and enables paging by setting CR3 and CR0.PG. Must be called after `pmm_init()`.

### `vmm_map_page`

```c
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags);
```

Maps a single page. If the page table for `virt` does not exist, allocates one via `pmm_alloc_frame`. Inserts the mapping and issues `invlpg` to invalidate the TLB entry.

`flags` is an OR of PTE flag bits (defined in `kernel/vmm.c`):

| Flag | Bit | Meaning |
|---|---|---|
| `VMM_PRESENT` | 0 | Page is present |
| `VMM_WRITABLE` | 1 | Page is writable |
| `VMM_USER` | 2 | User-mode access allowed |

### `vmm_get_mapping`

```c
phys_addr_t vmm_get_mapping(virt_addr_t virt);
```

Returns the physical address mapped to `virt`, or `0` if the page is not present.

---

## Kernel Heap

### `heap_init`

```c
void heap_init(void);
```

Initializes the heap allocator over an 8 MiB region starting at `__kernel_end`. Creates one free block spanning the entire heap. Must be called after `vmm_init()`.

### `kmalloc`

```c
void *kmalloc(size_t size);
```

Allocates at least `size` bytes from the kernel heap. Returns NULL if the heap is exhausted. Does not zero the returned memory. Splits the first fit block that is large enough; minimum remainder after split is 16 bytes.

### `kcalloc`

```c
void *kcalloc(size_t count, size_t size);
```

Allocates `count × size` bytes and zeroes the result. Calls `kmalloc` internally.

### `krealloc`

```c
void *krealloc(void *ptr, size_t size);
```

Resizes an allocation. If `ptr` is NULL, behaves as `kmalloc(size)`. If `size` is 0, frees `ptr` and returns NULL. Otherwise, allocates a new block, copies the smaller of old and new sizes, frees the old block, and returns the new pointer.

### `kfree`

```c
void kfree(void *ptr);
```

Frees a previously allocated block. Validates the heap magic value (`0xC0FFEE42`) and panics if it is corrupted. Coalesces adjacent free blocks.

### `heap_free_bytes`

```c
size_t heap_free_bytes(void);
```

Walks the heap block list and sums the sizes of all free blocks. O(n) in number of blocks.

### `heap_used_bytes`

```c
size_t heap_used_bytes(void);
```

Walks the heap block list and sums the sizes of all allocated blocks.
