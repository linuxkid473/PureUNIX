# PureUnix

A from-scratch, single-address-space operating system kernel for the i686 (IA-32) architecture, written in C99 and x86 assembly. PureUnix boots via Multiboot2, provides a FAT16 filesystem, a modal text editor, and an interactive shell with pipes and I/O redirection.

---

## Features

- **Boot**: Multiboot1 and Multiboot2 compatible; loads directly under GRUB or QEMU `-kernel`
- **Memory**: bitmap physical memory manager, identity-mapped paging, 8 MiB kernel heap
- **Interrupts**: full IDT with 32 CPU exception handlers and 16 IRQ stubs; INT 0x80 syscall gate
- **Drivers**: VGA text mode (80×25) with ANSI SGR colors, PS/2 keyboard, 16550 serial (COM1), ATA PIO disk
- **Filesystem**: FAT16 read/write with directories, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`
- **Shell**: interactive line editor with history (64 entries), tab completion, pipes (`|`), and I/O redirection (`<`, `>`, `>>`)
- **Editor**: modal vim-like editor (`vim`/`vi`) with NORMAL, INSERT, COMMAND, and search modes
- **Scheduler**: cooperative round-robin task scheduler with explicit `yield`
- **Syscalls**: `write`, `read`, `exit`, `getpid`, `yield` via INT 0x80
- **Userland**: ELF32 program loader; programs link against `libpure` for syscall access

---

## Implementation Status

| Subsystem | Status | Notes |
|---|---|---|
| Multiboot boot | Complete | Both Multiboot1 and Multiboot2 headers present |
| GDT (5 segments) | Complete | Null, kernel code/data, user code/data |
| IDT + interrupt stubs | Complete | 32 exceptions + 16 IRQs + syscall gate |
| PIC remapping | Complete | IRQ 0–15 → INT 32–47 |
| PIT (100 Hz timer) | Complete | Tick counter; `pit_sleep` busy-waits |
| Physical memory manager | Complete | Bitmap allocator, 128 MiB cap |
| Virtual memory / paging | Complete | Identity maps first 128 MiB |
| Kernel heap | Complete | Linked-list allocator with split and coalesce |
| VGA driver | Complete | 80×25 text mode, ANSI colors, hardware cursor |
| Serial driver | Complete | COM1 at 38400 baud, ANSI pass-through |
| PS/2 keyboard | Complete | Scan code set 1, shift/ctrl/caps, arrow keys |
| ATA PIO driver | Complete | Primary master, LBA28, sector read and write |
| FAT16 filesystem | Complete | Read, write, create, delete, rename, mkdir |
| VFS layer | Complete | Thin dispatch layer over FAT16 |
| ELF32 loader | Complete | Loads into kernel address space (0x400000–0x700000) |
| Cooperative scheduler | Complete | Round-robin; no preemption |
| Syscall interface | Partial | 5 syscalls; `exit` returns but does not terminate task |
| Shell builtins (27) | Complete | ls, cd, cat, echo, cp, mv, rm, mkdir, free, ps, kill, etc. |
| Shell pipeline/redirect | Complete | Up to 4 pipe stages, `<`, `>`, `>>` |
| vim-like editor | Complete | Modal edit, undo, search, `:w`/`:q`/`:wq` |
| Userland programs | Partial | `hello` is functional; other programs are stubs |
| User/kernel separation | Not implemented | ELF programs run in kernel address space |
| Preemptive scheduling | Not implemented | No timer-driven context switch |
| `TASK_SLEEPING` wakeup | Not implemented | State defined; no sleep/wakeup mechanism |
| `fork` / `exec` / `wait` | Not implemented | — |
| Network stack | Not implemented | — |

---

## Screenshots

*Placeholder. Boot PureUnix under QEMU to see the shell and editor.*

---

## Directory Layout

```
PureUnix/
├── arch/i386/       GDT, IDT, PIC, PIT, syscall, context switch
├── boot/            Multiboot entry stub, linker script, GRUB config
├── drivers/         VGA, keyboard, serial, ATA
├── editor/          In-kernel vim-like editor
├── fs/              FAT16 driver and VFS abstraction
├── include/pureunix/  Public headers
├── kernel/          main, PMM, VMM, heap, ELF loader, scheduler, panic, reboot
├── libc/            Freestanding string, printf, stdlib, ctype
├── shell/           Parser, line editor, builtins
├── tools/           mkfat16.py: FAT16 disk image builder
├── user/            Userland programs, crt0.S, libpure
├── build/           Build output (generated)
└── docs/            Documentation
```

---

## Building

### Dependencies

| Tool | Purpose |
|---|---|
| `i686-elf-gcc` | Cross-compiler targeting freestanding i686-elf |
| `python3` | Runs `tools/mkfat16.py` to build the disk image |
| `qemu-system-i386` | Emulation target |
| `grub-mkrescue` + `xorriso` | ISO creation (optional) |

See `docs/build.md` for full toolchain setup instructions.

### Build Commands

```sh
make          # build kernel ELF and FAT16 disk image
make run      # build and boot in QEMU
make iso      # build bootable ISO (requires grub-mkrescue)
make clean    # remove build output
```

---

## Running in QEMU

```sh
make run
```

Equivalent command:

```sh
qemu-system-i386 -m 128M \
    -kernel build/pureunix.elf \
    -drive file=build/pureunix.img,format=raw,if=ide,index=0 \
    -serial stdio -no-reboot -no-shutdown
```

Serial output (all `printf` output) appears on `stdio`. The VGA display appears in the QEMU window.

---

## First Commands

```sh
help
ls /
cat /README.TXT
touch note.txt
echo hello > note.txt
cat note.txt
mkdir docs
vim docs/todo.txt
hello
calculator
```

---

## Current Limitations

- No user/kernel memory protection — ELF programs execute in the kernel address space at ring 0.
- Cooperative scheduling only — a spinning task starves all others.
- `SYS_EXIT` returns the value in EBX but does not terminate the calling task.
- FAT16 filenames are restricted to 8.3 format; long file names are not supported.
- Heap is fixed at 8 MiB and does not grow.
- Physical memory is capped at 128 MiB.
- `printf` supports `%c %s %d %i %u %x %X %p %%` but no width, padding, or floating-point.
- Shell tab completion only searches the root directory `/`.
- Editor provides single-level undo only.
- `fat16_rename` cannot move entries between directories.

---

## Future Roadmap

- Hardware-enforced user/kernel separation (ring 3, TSS, separate page tables)
- Preemptive scheduling via PIT IRQ0
- Expanded syscall table: `open`, `close`, `stat`, `mmap`, `fork`, `exec`, `wait`
- `TASK_SLEEPING` with a timer-based wakeup mechanism
- Page fault handler and demand paging
- LFN (long filename) support in the FAT16 driver
- Dynamic heap expansion through VMM

---

## License

No license file is currently included. All rights reserved by the author unless otherwise noted.
