# Developer Guide

## Directory Map

- `boot/`: Multiboot headers, bootstrap assembly, linker script, GRUB config
- `arch/i386/`: x86 descriptor tables, interrupts, PIC/PIT, syscalls, context switch
- `kernel/`: main kernel services, memory, heap, tasks, ELF loader, panic/reboot
- `drivers/`: VGA, keyboard, ATA
- `fs/`: VFS and FAT16
- `libc/`: freestanding C helpers
- `shell/`: parser, line editor, built-ins, shell executor
- `editor/`: terminal text editor
- `user/`: demo ELF programs and tiny syscall library
- `tools/`: host-side FAT16 image builder
- `docs/`: architecture and developer documentation

## Milestones

1. Bootloader: Multiboot2 header and `_start`.
2. VGA output: text terminal, scrolling, cursor, ANSI basics.
3. GDT/IDT: protected-mode descriptors and interrupt dispatch.
4. Keyboard: IRQ1 scancode driver and key queue.
5. Memory manager: Multiboot memory map parsing and frame bitmap.
6. Paging: identity-mapped 32-bit paging.
7. Heap: splitting/coalescing allocator.
8. ATA driver: primary-master PIO sector I/O.
9. FAT16: writable 8.3 filesystem with directories.
10. Shell: parser, history, completion, redirection, built-ins.
11. System calls: `int 0x80` ABI.
12. ELF loader: ELF32 `PT_LOAD` support for demos.
13. Multitasking: cooperative task structures and context switch.
14. Text editor: Nano-like terminal editor.
15. Userland utilities: seeded ELF demos in `/bin`.

## Design Decisions

- Keep architecture-specific code under `arch/i386` to leave room for `arch/x86_64`.
- Use QEMU direct kernel loading for rapid local development, while preserving GRUB Multiboot2 compatibility.
- Start with identity paging so hardware drivers and early ELF loading are easy to reason about.
- Implement FAT16 before a richer VFS because it gives immediate persistent shell behavior.
- Use 8.3 filenames first; long filenames require checksum-linked LFN slots and are best added after the core filesystem is stable.
- Keep ELF demos simple until ring 3, per-process page tables, and process file descriptors are added.

## Next Work

- Add ring 3 transitions with TSS and user segments.
- Give each process a page directory.
- Replace in-memory shell pipelines with stream/file-descriptor based pipes.
- Add preemptive scheduling from the PIT tick.
- Implement FAT long filename create/read support.
- Add a serial console and automated boot smoke tests.

