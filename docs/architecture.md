# Architecture Overview

PureUnix is organized as a small layered kernel.

## Boot

`boot/multiboot2.S` provides:

- a Multiboot2 header for GRUB
- a Multiboot v1 header for QEMU direct `-kernel` loading
- a 32 KiB bootstrap stack
- the `_start` entry point

GRUB/QEMU enters the kernel in protected mode. The assembly entry disables interrupts, installs the stack, and calls `kernel_main(magic, mbi_addr)`.

## Kernel Initialization

`kernel/main.c` initializes subsystems in dependency order:

1. VGA console
2. GDT, IDT, and PIC
3. Physical memory manager
4. Paging
5. Kernel heap
6. Cooperative task list
7. Syscalls
8. PIT timer and keyboard IRQ
9. ATA disk driver
10. FAT16 root filesystem
11. Interactive shell

## Low-Level x86

The architecture layer lives in `arch/i386`.

- `gdt.c` creates flat kernel/user code and data segments.
- `idt.c` installs CPU exception, IRQ, and syscall gates.
- `interrupt_stubs.S` saves registers and calls the C dispatcher.
- `pic.c` remaps IRQs to vectors 32-47.
- `pit.c` configures the timer at 100 Hz.
- `context_switch.S` provides the primitive for cooperative scheduling.

The layout is deliberately i386-specific under `arch/i386` so future x86_64 code can live beside it instead of being tangled into generic kernel code.

## Memory

`kernel/pmm.c` parses Multiboot memory maps when available and tracks frames with a bitmap. It reserves low memory, the kernel image, and the bitmap itself.

`kernel/vmm.c` builds a page directory with identity mappings for the first 128 MiB and enables paging. This makes early drivers simple while giving the kernel a real paging foundation.

`kernel/heap.c` provides `kmalloc`, `kcalloc`, `krealloc`, and `kfree` using a splitting/coalescing free list.

## Storage

The ATA driver uses PIO on the primary IDE bus. The FAT16 driver mounts the first attached disk and exposes operations through the VFS:

- stat
- read file
- write/append/truncate file
- create file
- mkdir
- unlink/rmdir
- rename
- readdir

## Shell and Programs

The shell runs in kernel mode and provides Unix-like built-ins. Unknown commands are resolved as ELF executables under `/bin`.

ELF programs use a tiny user library that calls `int 0x80`. They are currently demos rather than isolated processes.

