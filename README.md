# PureUNIX

A from-scratch operating system kernel for the i686 (IA-32) architecture, written in C99 and x86 assembly. PureUNIX boots via GRUB (Multiboot1/2), provides a permission-aware, mount-table VFS over EXT2 and FAT16, runs ELF32 user programs in ring 3, and ships a modal text editor and an interactive shell with pipes and I/O redirection.

---

## Features

- **Boot**: Multiboot1 and Multiboot2 compatible; boots through GRUB2 (`make iso`/`make run`) with a VBE linear-framebuffer request, with a Multiboot1 fallback for loaders (e.g. QEMU `-kernel`) that don't set up a graphics mode
- **Memory**: bitmap physical memory manager, identity-mapped paging (first 128 MiB), 8 MiB kernel heap
- **Interrupts**: full IDT with 32 CPU exception handlers, 16 IRQ stubs, an `int $0x80` syscall gate, and a kernel-internal `int $0x81` process-termination trap
- **Protection**: ring 3 user-mode execution — a hardware TSS gives every task its own kernel stack, and the ELF load window is the only region marked user-accessible
- **Drivers**: VGA text mode (80×25) with ANSI SGR colors, VBE linear framebuffer + boot splash, PS/2 keyboard, 16550 serial (COM1), ATA PIO disk (primary master and slave)
- **Filesystems**: EXT2 read/write driver (primary root filesystem, ATA slave) with symlinks, hard links, and Unix permissions; FAT16 read/write driver (compatibility/testing store, ATA master)
- **VFS**: mount-table router with longest-prefix path resolution, symlink-following path resolution, and Unix permission enforcement (`uid`/`gid`/mode) on every call
- **Shell**: interactive line editor with history (64 entries), tab completion, pipes (`|`), I/O redirection (`<`, `>`, `>>`), and 29 builtins
- **Editor**: modal vim-like editor (`vim`/`vi`) with NORMAL, INSERT, COMMAND, and search modes
- **Scheduler**: cooperative round-robin task scheduler; `elf_exec` spawns a ring-3 task per program and blocks until it exits
- **Syscalls**: 21 syscalls via `int $0x80` — process (`exit`, `getpid`, `yield`), I/O (`read`, `write`), files (`open`, `close`, `lseek`, `stat`, `lstat`, `access`, `readdir`), and filesystem mutation (`chmod`, `chown`, `mkdir`, `unlink`, `rmdir`, `rename`, `link`, `symlink`, `readlink`)
- **Userland**: ELF32 program loader running programs in ring 3 on their own stack; programs link against `libpure` for syscall access

---

## Implementation Status

| Subsystem | Status | Notes |
|---|---|---|
| Multiboot boot | Complete | Multiboot1 + Multiboot2 headers; GRUB2 boot path with VBE framebuffer tag |
| GDT (6 descriptors) | Complete | Null, kernel code/data, user code/data, TSS |
| IDT + interrupt stubs | Complete | 32 exceptions + 16 IRQs + syscall gate (`0x80`) + termination trap (`0x81`) |
| PIC remapping | Complete | IRQ 0–15 → INT 32–47 |
| PIT (100 Hz timer) | Complete | Tick counter; `pit_sleep` busy-waits; no preemption |
| Physical memory manager | Complete | Bitmap allocator, 128 MiB cap |
| Virtual memory / paging | Complete | Identity maps first 128 MiB; per-page `PAGE_USER` for the ELF window |
| Kernel heap | Complete | Linked-list allocator with split and coalesce |
| VGA driver | Complete | 80×25 text mode, ANSI colors, hardware cursor |
| VBE framebuffer + boot splash | Complete | Linear framebuffer parsed from the Multiboot2 tag; logo shown at boot |
| Serial driver | Complete | COM1, ANSI pass-through, mirrors all VGA output |
| PS/2 keyboard | Complete | Scan code set 1, shift/ctrl/caps, arrow keys |
| ATA PIO driver | Complete | Primary master + slave, LBA28, sector read/write |
| FAT16 filesystem | Complete | Read, write, create, delete, rename, mkdir; compatibility/testing store at `/fat` |
| EXT2 filesystem | Complete (read/write) | Superblock, BGDT, inode table, direct + singly-indirect blocks, allocation/freeing, symlinks, hard links; primary root filesystem at `/` |
| VFS layer | Complete | Mount-table routing (longest-prefix match), symlink-following path resolution, Unix permission enforcement |
| Ring 3 ELF32 execution | Complete | `elf_exec` loads into a fixed window (`0x400000`–`0x700000`), spawns a ring-3 task via a hardware TSS + `iret`, and blocks in `task_join()` until it exits |
| Cooperative scheduler | Complete | Round-robin; no preemption; `task_join()` lets a caller wait on a spawned task |
| Syscall interface | Complete (21 syscalls + 1 test-only) | See Features above; `SYS_DEBUG_SETCRED` is test-only, no privilege check |
| Shell builtins (29) | Complete | ls, cd, cat, echo, cp, mv, rm, mkdir, rmdir, touch, stat, mount, free, ps, kill, reboot, shutdown, env, export, etc. |
| Shell pipeline/redirect | Complete | Up to 4 pipe stages, `<`, `>`, `>>` |
| vim-like editor | Complete | Modal edit, undo, search, `:w`/`:q`/`:wq` |
| Userland programs | Partial | `hello`, `calc`, and the full syscall/filesystem regression suites (`opentest`, `readtest`, `ext2test`, `systest`) are functional; `viewer`/`sh`/`editor` are stubs (the real shell and editor run as kernel builtins) |
| Per-process address space | Not implemented | All programs share one page directory; only the kernel-vs-user boundary is enforced, not process-vs-process |
| Preemptive scheduling | Not implemented | No timer-driven context switch |
| `TASK_SLEEPING` wakeup | Not implemented | State defined; no sleep/wakeup mechanism |
| `fork` / real `exec` (user-triggered) | Not implemented | `elf_exec` is a kernel function called by the shell; no syscall lets a user program launch another |
| Network stack | Not implemented | — |

---

## Directory Layout

```
PureUNIX/
├── arch/i386/       GDT+TSS, IDT, PIC, PIT, syscall, context switch, ring3 entry (usermode.S)
├── boot/            Multiboot entry stub, linker script, GRUB config
├── drivers/         VGA, VBE framebuffer, boot splash, keyboard, serial, ATA (master + slave)
├── editor/          In-kernel vim-like editor
├── fs/
│   ├── ext2/        EXT2 read/write driver (superblock, BGDT, inode, dir, file, alloc/write, block cache)
│   ├── fat16.c      FAT16 read/write driver
│   └── vfs.c        Mount-table VFS: path resolution, symlinks, permissions
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
| `grub-mkrescue` + `xorriso` | ISO creation — required for `make run`/`make iso` (real GRUB is what sets up the VBE framebuffer; QEMU's built-in `-kernel` Multiboot1 loader never does) |

### Build Commands

```sh
make          # build kernel ELF, FAT16 disk image, and EXT2 disk image
make run      # build the ISO + both disks and boot in QEMU through GRUB
make iso      # build bootable ISO only (requires grub-mkrescue)
make clean    # remove build output
```

---

## Running in QEMU

```sh
make run
```

Equivalent command:

```sh
qemu-system-i386 -m 128M -cdrom build/pureunix.iso -boot d \
    -drive file=build/pureunix.img,format=raw,if=ide,index=0 \
    -drive file=build/ext2.img,format=raw,if=ide,index=1 \
    -serial stdio -no-reboot -no-shutdown
```

- `index=0` (ATA primary master): FAT16, mounted at `/fat` — compatibility/testing store
- `index=1` (ATA primary slave): EXT2, mounted at `/` — primary root filesystem (`/README.TXT`, `/etc/`, `/home/`, `/bin/`, `/testdir/`, `/bigfile.bin`, `/hugefile.bin`)

Every path resolves to exactly one mount via longest-prefix match — `/` and `/fat` are two separate trees, not a merged view. Serial output (all `printf` output) appears on `stdio`. The VGA/framebuffer display appears in the QEMU window.

---

## First Commands

```sh
help
ls /                        # EXT2 root
cat /README.TXT             # reads from EXT2
ls /bin                     # EXT2's /bin — where installed ELF programs live
ls /fat                     # FAT16's root (separate mount, compatibility/testing)
cat /etc/passwd             # reads from EXT2
touch note.txt
echo hello > note.txt
cat note.txt
mkdir docs
vim docs/todo.txt
hello                       # runs /bin/hello.elf in ring 3
systest                     # runs /bin/systest.elf — 203-case syscall/filesystem regression suite
ext2test                    # runs /bin/ext2test.elf — EXT2-specific syscall test
```

---

## Current Limitations

- User programs run in ring 3, but all share one page directory — there is no per-process address space, so programs are isolated from the kernel but not from each other (only one ever runs at a time, since there's no `fork`/user-triggered `exec`).
- Cooperative scheduling only — a spinning task starves all others.
- `SYS_EXIT` returns the value in EBX but does not terminate the calling task (this is intentional and tested — see `docs/syscalls.md`); a program only actually exits via the `int $0x81` trap `crt0` issues after `main()` returns.
- No page fault handler — an out-of-bounds access from a well-behaved program is fine, but a genuinely faulting one panics the kernel rather than being killed cleanly.
- FAT16 filenames are restricted to 8.3 format; long file names are not supported.
- EXT2 does not support doubly- or triply-indirect blocks (maximum file size ≈ 268 KiB with 1 KB blocks).
- Heap is fixed at 8 MiB and does not grow.
- Physical memory is capped at 128 MiB.
- `printf` supports `%c %s %d %i %u %x %X %p %%` but no width, padding, or floating-point.
- Shell tab completion only searches the root directory `/`.
- Editor provides single-level undo only.
- `fat16_rename` cannot move entries between directories.

---

## Future Roadmap

- Per-process address space (separate page directories, `fork`/`exec`/`wait`)
- Preemptive scheduling via PIT IRQ0
- A page fault handler, so a misbehaving user program is killed instead of panicking the kernel
- Expanded syscall table: `mmap`, `fork`, `exec`, `wait`
- `TASK_SLEEPING` with a timer-based wakeup mechanism
- Demand paging
- LFN (long filename) support in the FAT16 driver
- Doubly-indirect and triply-indirect block support in EXT2
- Dynamic heap expansion through VMM
