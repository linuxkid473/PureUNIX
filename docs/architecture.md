# Architecture

## Overview

PureUnix is a monolithic, single-address-space kernel for the i686 (IA-32) architecture. There is no hardware-enforced user/kernel separation in the current implementation: ELF user programs are loaded into the same virtual address space as the kernel and executed at ring 0.

The kernel is a single flat ELF binary linked at the 1 MiB physical address mark. It initializes protected-mode services sequentially from `kernel_main`, then enters the interactive shell loop. There is no background thread or idle process: the kernel spins on `hlt` in a loop placed after `shell_run`, which never returns in normal operation.

---

## Boot Flow

```
GRUB / QEMU
    │
    └── boot/multiboot2.S  (_start)
            │  cli
            │  ESP = boot_stack_top   (32 KiB static stack in .bss)
            │  EBP = 0
            │  push EBX              (Multiboot info address)
            │  push EAX              (Multiboot magic)
            └── call kernel_main
```

```
kernel_main(magic, mbi_addr)
    ├── serial_init()            COM1, 38400 baud
    ├── vga_init()               80×25 text mode, clear screen
    ├── arch_init()
    │       ├── gdt_init()       Load 5-entry GDT, reload segment registers, far jump
    │       ├── idt_init()       Install 256-entry IDT (exceptions, IRQs, INT 0x80)
    │       └── pic_init()       Remap 8259A: IRQ 0-7 → INT 32-39, IRQ 8-15 → INT 40-47
    ├── pmm_init(magic, mbi)     Parse Multiboot mmap; initialize frame bitmap
    ├── vmm_init()               Identity map 128 MiB; enable paging (CR0.PG)
    ├── heap_init()              8 MiB linked-list heap at __kernel_end
    ├── tasking_init()           Create "kernel" task; set as current
    ├── syscall_init()           No-op (INT 0x80 gate already set in idt_init)
    ├── pit_init(100)            Program PIT channel 0 to 100 Hz; enable IRQ0
    ├── keyboard_init()          Flush PS/2 buffer; register IRQ1 handler; enable IRQ1
    ├── ata_init()               Probe ATA primary master; register IRQ14 handler
    ├── vfs_init()               Initialize VFS error string
    ├── fat16_mount(disk)        Parse BPB from sector 0 of ATA disk (if present)
    ├── arch_enable_interrupts() STI
    └── shell_run()              Enter interactive shell (does not return)
```

---

## Kernel Subsystems

| Subsystem | Location | Responsibility |
|---|---|---|
| Boot stub | `boot/` | Multiboot headers, initial stack, `_start`, linker script |
| Architecture | `arch/i386/` | GDT, IDT, PIC, PIT, syscall dispatch, context switch |
| Physical memory | `kernel/pmm.c` | Bitmap frame allocator, Multiboot1/2 mmap parsing |
| Virtual memory | `kernel/vmm.c` | Page directory, identity mapping, `vmm_map_page` |
| Kernel heap | `kernel/heap.c` | `kmalloc`/`kfree`/`krealloc`, split/coalesce allocator |
| Task scheduler | `kernel/task.c` | Cooperative round-robin, context switch, task lifecycle |
| ELF loader | `kernel/elf.c` | Validate and load ELF32 executables from VFS |
| Panic | `kernel/panic.c` | Print message in red on white; halt CPU |
| Reboot/shutdown | `kernel/reboot.c` | Keyboard controller reset; ACPI/QEMU shutdown port writes |
| VGA | `drivers/vga.c` | 80×25 text mode, ANSI SGR, hardware cursor |
| Serial | `drivers/serial.c` | COM1 output, ANSI cursor control |
| Keyboard | `drivers/keyboard.c` | PS/2 scan code set 1 → key codes, ring buffer |
| ATA | `drivers/ata.c` | PIO read/write, primary master, LBA28 |
| FAT16 | `fs/fat16.c` | BPB parse, path lookup, cluster I/O, all file operations |
| VFS | `fs/vfs.c` | Dispatch wrapper over FAT16; path normalization |
| libc | `libc/` | `printf`, string functions, `ctype`, `stdlib` |
| Shell | `shell/` | Line editor, parser, pipeline execution, 27 builtins |
| Editor | `editor/editor.c` | vim-like modal editor, 512 lines, single-level undo |

---

## Memory Layout

```
Physical / Virtual Address (identity mapped 1:1)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
0x0000_0000 – 0x000F_FFFF   Low memory (reserved: IVT, BDA, VGA ROM, BIOS)

0x0010_0000 (__kernel_start)
    [.multiboot]             Multiboot1 + Multiboot2 headers (aligned 8)
    [.text]                  Kernel code (4 KiB aligned)
    [.rodata]                Read-only data, exception names
    [.data]                  Initialized globals
    [.bss]                   Zero globals; boot_stack_bottom–boot_stack_top (32 KiB)
(__kernel_end)               End of kernel image (linker symbol)

ALIGN(__kernel_end, 4096)    Kernel heap start
                             8 MiB fixed heap (kmalloc / kfree)

0x000B_8000                  VGA text framebuffer (within identity-mapped region)

0x0040_0000 – 0x0070_0000   Valid ELF user program load range (enforced by elf_exec)

0x0800_0000                  End of identity-mapped region (128 MiB)
```

The VMM identity-maps 32 page tables, each covering 4 MiB (1024 × 4 KiB entries), for a total of 128 MiB. All physical addresses in this range are directly accessible at the same virtual address.

---

## User / Kernel Separation

**Current state: none.** ELF32 executables are loaded by `elf_exec` directly into the kernel virtual address space at their `p_vaddr` (validated to be within `0x400000–0x700000`). The entry point is called as a C function pointer. Programs run at ring 0 with full kernel privileges.

The GDT does define user code (selector `0x18`, DPL=3) and user data (selector `0x20`, DPL=3) segments. The INT 0x80 gate is installed with DPL=3 (flags `0xEE`), allowing ring-3 callers. However, no mechanism yet transitions execution to ring 3 or enforces separate page tables.

---

## Initialization Order

The order in `kernel_main` is not arbitrary:

1. **Serial before VGA**: serial output works from the first line, before VGA state is set.
2. **arch_init before pmm_init**: GDT and IDT must be valid before any potential fault. PIC remapping prevents spurious interrupts from racing with IDT installation.
3. **pmm_init before vmm_init**: the VMM calls `pmm_alloc_frame` to create page table pages.
4. **vmm_init before heap_init**: the heap region must be mapped before any pointer in it is dereferenced.
5. **heap_init before tasking_init**: task stacks are allocated via `kmalloc`.
6. **pit_init/keyboard_init before arch_enable_interrupts**: handlers must be registered before the CPU can receive them.
7. **ata_init + fat16_mount before shell_run**: shell builtins like `ls` and `cat` assume the filesystem is ready.

---

## Module Dependencies

```
kernel_main
├── serial_init, vga_init          (leaf: only io.h)
├── arch_init
│   ├── gdt_init     needs: string.h (memset)
│   ├── idt_init     needs: string.h, io.h, panic.h, syscall.h
│   └── pic_init     needs: io.h
├── pmm_init         needs: memory.h, multiboot.h, string.h, stdio.h
├── vmm_init         needs: memory.h (pmm_alloc_frame), string.h
├── heap_init        needs: memory.h, string.h
├── tasking_init     needs: memory.h, string.h
├── pit_init         needs: io.h, arch.h
├── keyboard_init    needs: io.h, arch.h
├── ata_init         needs: io.h, arch.h, disk.h
├── vfs_init         needs: fat16.h, string.h
├── fat16_mount      needs: disk.h, memory.h, string.h, stdio.h
└── shell_run
    ├── shell_readline   needs: keyboard.h, vga.h, stdio.h, vfs.h
    ├── shell_parse      needs: string.h
    ├── builtins         needs: vfs.h, fat16.h, memory.h, task.h, elf.h, editor.h
    └── elf_exec         needs: vfs.h, memory.h, string.h
```
