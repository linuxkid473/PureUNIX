# Graph Report - .  (2026-06-29)

## Corpus Check
- Corpus is ~17,953 words - fits in a single context window. You may not need a graph.

## Summary
- 428 nodes · 1256 edges · 18 communities (17 shown, 1 thin omitted)
- Extraction: 73% EXTRACTED · 27% INFERRED · 0% AMBIGUOUS · INFERRED: 336 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Keyboard Input Handling|Keyboard Input Handling]]
- [[_COMMUNITY_Interrupt & IDT Infrastructure|Interrupt & IDT Infrastructure]]
- [[_COMMUNITY_Shell & Builtins|Shell & Builtins]]
- [[_COMMUNITY_FAT16 Filesystem Driver|FAT16 Filesystem Driver]]
- [[_COMMUNITY_Serial & VGA Console IO|Serial & VGA Console I/O]]
- [[_COMMUNITY_GDT & Syscall Dispatch|GDT & Syscall Dispatch]]
- [[_COMMUNITY_Architecture Overview Docs|Architecture Overview Docs]]
- [[_COMMUNITY_Virtual Filesystem Layer|Virtual Filesystem Layer]]
- [[_COMMUNITY_Physical Memory Manager|Physical Memory Manager]]
- [[_COMMUNITY_FAT16 Image Builder Tool|FAT16 Image Builder Tool]]
- [[_COMMUNITY_User Programs & libpure|User Programs & libpure]]
- [[_COMMUNITY_C Standard Library (ctype)|C Standard Library (ctype)]]
- [[_COMMUNITY_Shell Command Parser|Shell Command Parser]]
- [[_COMMUNITY_Cross-Compilation Toolchain|Cross-Compilation Toolchain]]

## God Nodes (most connected - your core abstractions)
1. `shell_out_printf()` - 24 edges
2. `memset()` - 22 edges
3. `kernel_main()` - 19 edges
4. `strlen()` - 19 edges
5. `Fat16` - 19 edges
6. `shell_out_puts()` - 17 edges
7. `path_lookup()` - 16 edges
8. `outb()` - 16 edges
9. `kfree()` - 16 edges
10. `printf()` - 16 edges

## Surprising Connections (you probably didn't know these)
- `find_in_dir()` --calls--> `memcmp()`  [INFERRED]
  fs/fat16.c → libc/string.c
- `cmd_ps()` --calls--> `task_list()`  [INFERRED]
  shell/builtins.c → kernel/task.c
- `cmd_kill()` --calls--> `task_kill()`  [INFERRED]
  shell/builtins.c → kernel/task.c
- `gdt_init()` --calls--> `memset()`  [INFERRED]
  arch/i386/gdt.c → libc/string.c
- `idt_init()` --calls--> `memset()`  [INFERRED]
  arch/i386/idt.c → libc/string.c

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Kernel Boot and Initialization Sequence** — docs_architecture_boot_layer, docs_architecture_kernel_init, docs_architecture_gdt, docs_architecture_idt, docs_architecture_pic, docs_architecture_pmm, docs_architecture_vmm, docs_architecture_heap [EXTRACTED 1.00]
- **Kernel Memory Management Subsystem** — docs_architecture_pmm, docs_architecture_vmm, docs_architecture_heap [INFERRED 0.95]
- **Storage I/O Stack** — docs_architecture_ata_driver, docs_architecture_fat16_driver, docs_architecture_vfs, docs_filesystem_vfs_operations [EXTRACTED 1.00]

## Communities (18 total, 1 thin omitted)

### Community 0 - "Keyboard Input Handling"
Cohesion: 0.08
Nodes (50): interrupt_regs_t, extended_key(), keyboard_getkey(), keyboard_irq(), keyboard_try_getkey(), push_key(), delete_char(), editor_open() (+42 more)

### Community 1 - "Interrupt & IDT Infrastructure"
Cohesion: 0.08
Nodes (43): arch_init(), interrupt_regs_t, idt_init(), idt_set_gate(), interrupt_register_handler(), isr_dispatch(), irq_disable(), irq_enable() (+35 more)

### Community 2 - "Shell & Builtins"
Cohesion: 0.17
Nodes (46): strchr(), abs_path(), bcd(), shell_command_t, shell_context_t, shell_output_t, task_t, vfs_dirent_t (+38 more)

### Community 3 - "FAT16 Filesystem Driver"
Cohesion: 0.16
Nodes (43): fat_dir_entry_t, found_entry_t, alloc_cluster(), disk_device_t, vfs_readdir_cb_t, vfs_stat_t, cluster_size(), cluster_to_lba() (+35 more)

### Community 4 - "Serial & VGA Console I/O"
Cohesion: 0.12
Nodes (33): serial_clear(), serial_erase_line(), serial_move_cursor(), serial_putc(), serial_write(), serial_write_uint(), ansi_execute(), ansi_sgr() (+25 more)

### Community 5 - "GDT & Syscall Dispatch"
Cohesion: 0.10
Nodes (14): gdt_init(), gdt_set_gate(), interrupt_regs_t, syscall_dispatch(), elf_exec(), elf_is_valid(), task_t, next_ready_task() (+6 more)

### Community 6 - "Architecture Overview Docs"
Cohesion: 0.09
Nodes (30): ATA PIO Driver, Boot Layer (Multiboot2 / _start), Context Switch Primitive, Cooperative Task Scheduler, ELF32 Executable Loader, FAT16 Filesystem Driver, Global Descriptor Table (GDT), Kernel Heap Allocator (+22 more)

### Community 7 - "Virtual Filesystem Layer"
Cohesion: 0.12
Nodes (27): builtin_t, fat16_is_mounted(), vfs_readdir_cb_t, vfs_stat_t, set_error(), vfs_create(), vfs_init(), vfs_mkdir() (+19 more)

### Community 8 - "Physical Memory Manager"
Cohesion: 0.17
Nodes (20): phys_addr_t, frame_clear(), frame_set(), frame_test(), mark_region_free(), parse_multiboot1(), parse_multiboot2(), pmm_alloc_frame() (+12 more)

### Community 9 - "FAT16 Image Builder Tool"
Cohesion: 0.24
Nodes (4): Fat16, le16(), le32(), main()

### Community 10 - "User Programs & libpure"
Cohesion: 0.16
Nodes (11): main(), main(), main(), pu_puti(), pu_puts(), pu_read(), pu_strlen(), pu_write() (+3 more)

### Community 11 - "C Standard Library (ctype)"
Cohesion: 0.27
Nodes (11): isalnum(), isalpha(), isdigit(), islower(), isspace(), isupper(), tolower(), toupper() (+3 more)

### Community 12 - "Shell Command Parser"
Cohesion: 0.36
Nodes (8): shell_command_t, shell_output_t, command_init(), is_meta(), next_token(), shell_error(), shell_parse(), shell_pipeline_t

## Knowledge Gaps
- **11 isolated node(s):** `Boot Layer (Multiboot2 / _start)`, `Programmable Interval Timer (PIT)`, `Kernel Heap Allocator`, `VGA Text Console`, `i686-elf Cross-Compilation Toolchain` (+6 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `memset()` connect `FAT16 Filesystem Driver` to `Keyboard Input Handling`, `Interrupt & IDT Infrastructure`, `GDT & Syscall Dispatch`, `Virtual Filesystem Layer`, `Physical Memory Manager`, `Shell Command Parser`?**
  _High betweenness centrality (0.042) - this node is a cross-community bridge._
- **Why does `kernel_main()` connect `Interrupt & IDT Infrastructure` to `Keyboard Input Handling`, `FAT16 Filesystem Driver`, `Serial & VGA Console I/O`, `GDT & Syscall Dispatch`, `Virtual Filesystem Layer`, `Physical Memory Manager`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._
- **Why does `printf()` connect `Interrupt & IDT Infrastructure` to `Keyboard Input Handling`, `FAT16 Filesystem Driver`, `Serial & VGA Console I/O`, `GDT & Syscall Dispatch`, `Virtual Filesystem Layer`, `Physical Memory Manager`?**
  _High betweenness centrality (0.023) - this node is a cross-community bridge._
- **Are the 20 inferred relationships involving `shell_out_printf()` (e.g. with `cmd_cat()` and `cmd_cd()`) actually correct?**
  _`shell_out_printf()` has 20 INFERRED edges - model-reasoned connections that need verification._
- **Are the 21 inferred relationships involving `memset()` (e.g. with `gdt_init()` and `idt_init()`) actually correct?**
  _`memset()` has 21 INFERRED edges - model-reasoned connections that need verification._
- **What connects `Boot Layer (Multiboot2 / _start)`, `Programmable Interval Timer (PIT)`, `Kernel Heap Allocator` to the rest of the system?**
  _14 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Keyboard Input Handling` be split into smaller, more focused modules?**
  _Cohesion score 0.07864488808227466 - nodes in this community are weakly interconnected._