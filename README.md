# PureUNIX

A from-scratch, single-address-space operating system kernel for the i686 (IA-32) architecture, written in C99 and x86 assembly. PureUNIX boots via Multiboot2, provides a dual-filesystem VFS (EXT2 + FAT16), a modal text editor, and an interactive shell with pipes and I/O redirection.

---

## Features

- **Boot**: Multiboot1 and Multiboot2 compatible; loads directly under GRUB or QEMU `-kernel`
- **Memory**: bitmap physical memory manager, identity-mapped paging, 8 MiB kernel heap
- **Interrupts**: full IDT with 32 CPU exception handlers and 16 IRQ stubs; INT 0x80 syscall gate
- **Drivers**: VGA text mode (80×25) with ANSI SGR colors, PS/2 keyboard, 16550 serial (COM1), ATA PIO disk (primary master and slave)
- **Filesystems**: EXT2 read-only driver (data disk, ATA slave); FAT16 read/write (program store, ATA master)
- **VFS**: dual-dispatch layer — reads and directory listings merge entries from both EXT2 and FAT16; writes go exclusively to FAT16
- **Shell**: interactive line editor with history (64 entries), tab completion, pipes (`|`), and I/O redirection (`<`, `>`, `>>`)
- **Editor**: modal vim-like editor (`vim`/`vi`) with NORMAL, INSERT, COMMAND, and search modes
- **Scheduler**: cooperative round-robin task scheduler with explicit `yield`
- **Syscalls**: `write`, `read` (stdin + VFS file descriptors), `exit`, `getpid`, `yield`, `open`, `close`, `lseek`, `stat` via INT 0x80
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
| ATA PIO driver | Complete | Primary master + slave, LBA28, sector read/write |
| FAT16 filesystem | Complete | Read, write, create, delete, rename, mkdir |
| EXT2 filesystem | Complete (read-only) | Superblock, BGDT, inode table, direct + singly-indirect blocks, directory traversal |
| VFS layer | Complete | Dual-dispatch: EXT2-first reads; union readdir; FAT16-only writes; path normalization |
| ELF32 loader | Complete | Loads into kernel address space (0x400000–0x700000) |
| Cooperative scheduler | Complete | Round-robin; no preemption |
| Syscall interface | Complete (9 syscalls) | `write`, `read` (stdin + file fd), `exit`, `getpid`, `yield`, `open`, `close`, `lseek`, `stat` |
| Shell builtins (27) | Complete | ls, cd, cat, echo, cp, mv, rm, mkdir, free, ps, kill, etc. |
| Shell pipeline/redirect | Complete | Up to 4 pipe stages, `<`, `>`, `>>` |
| vim-like editor | Complete | Modal edit, undo, search, `:w`/`:q`/`:wq` |
| Userland programs | Partial | `hello`, file syscall tests functional; viewer/sh/editor are stubs |
| User/kernel separation | Not implemented | ELF programs run in kernel address space |
| Preemptive scheduling | Not implemented | No timer-driven context switch |
| `TASK_SLEEPING` wakeup | Not implemented | State defined; no sleep/wakeup mechanism |
| `fork` / `exec` / `wait` | Not implemented | — |
| Network stack | Not implemented | — |

---

## Directory Layout

```
PureUNIX/
├── arch/i386/       GDT, IDT, PIC, PIT, syscall, context switch
├── boot/            Multiboot entry stub, linker script, GRUB config
├── drivers/         VGA, keyboard, serial, ATA (master + slave)
├── editor/          In-kernel vim-like editor
├── fs/
│   ├── ext2/        EXT2 read-only driver (superblock, BGDT, inode, dir, file, block cache)
│   ├── fat16.c      FAT16 read/write driver
│   └── vfs.c        Dual-dispatch VFS abstraction
├── include/pureunix/  Public headers
├── kernel/          main, PMM, VMM, heap, ELF loader, scheduler, panic, reboot
├── libc/            Freestanding string, printf, stdlib, ctype
├── shell/           Parser, line editor, builtins
├── tools/           mkfat16.py: FAT16 image builder; mkext2.py: EXT2 image builder
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
| `python3` | Runs `tools/mkfat16.py` and `tools/mkext2.py` to build disk images |
| `qemu-system-i386` | Emulation target |
| `grub-mkrescue` + `xorriso` | ISO creation (optional) |

### Build Commands

```sh
make          # build kernel ELF, FAT16 disk image, and EXT2 disk image
make run      # build and boot in QEMU with both disks attached
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
    -drive file=build/ext2.img,format=raw,if=ide,index=1 \
    -serial stdio -no-reboot -no-shutdown
```

- `index=0` (ATA primary master): FAT16 — program store (`/bin/*.elf`)
- `index=1` (ATA primary slave): EXT2 — data filesystem (`/README.TXT`, `/etc/`, `/home/`, `/testdir/`, `/bigfile.bin`, `/hugefile.bin`)

Serial output (all `printf` output) appears on `stdio`. The VGA display appears in the QEMU window.

---

## First Commands

```sh
help
ls /                        # shows entries from both EXT2 and FAT16
cat /README.TXT             # reads from EXT2
ls /bin                     # reads from FAT16
cat /etc/passwd             # reads from EXT2
touch note.txt
echo hello > note.txt
cat note.txt
mkdir docs
vim docs/todo.txt
hello                       # runs /bin/hello.elf from FAT16
ext2test                    # runs /bin/ext2test.elf — 14-case EXT2 syscall test
```

---

## Current Limitations

- No user/kernel memory protection — ELF programs execute in the kernel address space at ring 0.
- Cooperative scheduling only — a spinning task starves all others.
- `SYS_EXIT` returns the value in EBX but does not terminate the calling task.
- FAT16 filenames are restricted to 8.3 format; long file names are not supported.
- EXT2 is read-only; writes always go to FAT16.
- EXT2 does not support doubly- or triply-indirect blocks (maximum file size ≈ 268 KiB with 1 KB blocks).
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
- Expanded syscall table: `mmap`, `fork`, `exec`, `wait`
- `TASK_SLEEPING` with a timer-based wakeup mechanism
- Page fault handler and demand paging
- EXT2 write support (journal-less, like ext2 revision 0)
- LFN (long filename) support in the FAT16 driver
- Doubly-indirect and triply-indirect block support in EXT2
- Dynamic heap expansion through VMM
