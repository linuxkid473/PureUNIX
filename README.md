# PureUnix

PureUnix is a small 32-bit x86 hobby operating system written mostly in C with the minimum assembly needed for boot, descriptor loading, interrupt stubs, and context switching.

It currently boots as an i686 Multiboot/Multiboot2 kernel, initializes protected-mode kernel services, mounts a FAT16 disk image, and starts an interactive shell named `sh`.

## Features

- Multiboot2-compatible kernel for GRUB, plus a Multiboot header for direct QEMU loading
- VGA text terminal with scrolling, cursor movement, backspace, and basic ANSI color/control support
- GDT, IDT, PIC, PIT, keyboard IRQ driver
- Physical memory manager, paging, and kernel heap allocator
- ATA PIO primary-master disk driver
- Writable FAT16 filesystem with VFS wrapper
- ELF32 executable loader for simple demo programs
- System call interrupt at `int 0x80`
- Cooperative task list and context-switch primitive
- Shell with history, tab completion, environment variables, pipes, redirection, and Unix-like commands
- Nano-style terminal editor opened with `nano FILE`
- Seeded FAT16 disk image containing `/README.TXT` and `/BIN/*.ELF`

## Quick Start

Requirements:

- `i686-elf-gcc`
- `i686-elf-as`
- `make`
- `python3`
- `qemu-system-i386`
- Optional for ISO images: `grub-mkrescue`

Build:

```sh
make
```

Run:

```sh
make run
```

Create a GRUB ISO:

```sh
make iso
```

`make iso` requires `grub-mkrescue`. `make run` uses QEMU's direct kernel loader and attaches the generated FAT16 disk image.

## First Commands

Inside PureUnix:

```sh
help
ls /
cat /README.TXT
touch note.txt
echo hello > note.txt
cat note.txt
mkdir docs
nano docs/todo.txt
hello
calculator
```

## Current Limits

This is a coherent hobby OS baseline, not a production Unix:

- FAT16 currently supports 8.3 filenames only; long filename entries are skipped.
- User ELF programs are loaded into a fixed low virtual address range and run in ring 0 for now.
- The scheduler is cooperative; PIT preemption is intentionally left as a later step.
- The shell implements pipelines through in-memory buffers, so large streaming pipelines are bounded.
- Virtual memory identity maps the first 128 MiB to keep early drivers and the ELF demo loader simple.

