# PureUNIX — A Modern i386 Operating System

A complete from-scratch operating system kernel for the i686 (IA-32) architecture, written in C99 and x86 assembly. PureUNIX boots via GRUB (Multiboot1/2), provides a permission-aware, mount-table VFS over EXT2 and FAT16, runs ELF32 user programs in ring 3 with preemptive scheduling and multi-process job control, and ships a graphical window manager (PUDE), file manager, real network stack, and a growing ecosystem of real-world UNIX applications including SQLite, ncurses, htop, SDL2, Chocolate Doom, and Qt6.

<img width="640" height="428" alt="image" src="https://github.com/user-attachments/assets/e118e4e2-9673-4286-8c20-69bb029f1ecc" />

---

## What Makes PureUNIX Unique

PureUNIX is not a Linux clone or minimal POSIX toy — it's a **complete, real operating system** that can boot, run graphical applications, and play games. Key differentiators:

1. **Real Hardware Support**: Boots and runs on real x86 hardware (HP Pavilion tested), not just QEMU. Includes real e1000 NIC, xHCI USB, and AHCI-class storage drivers.

2. **From-Scratch Kernel**: Every line of kernel code is written from scratch in C99 and x86 assembly. No Linux code borrowed, no microkernel architecture — just a clean, minimal i386 kernel that fits one person's understanding.

3. **Full POSIX Semantics**: `fork()`/`exec()`/`wait()`, process groups, sessions, job control, signals, PTY subsystem, and `fcntl()` locking — all working for real UNIX applications. Not a toy shell with exec-only semantics.

4. **Graphics & Interactivity**: SDL2, Qt6, and a graphical window manager (PUDE) with drag/resize/dock support. Real applications like Chocolate Doom, SQLite, and ncurses run without modification. Not just text mode or framebuffer dumps.

5. **Real Multiboot2 Boot Path**: Uses GRUB2 to negotiate VBE graphics mode and load real MBR+GRUB+EXT2 disk images. The ISO boots on real hardware via BIOS/UEFI compatibility (legacy mode), not just QEMU's `-kernel` shortcut.

6. **Persistent Storage**: EXT2 filesystem with multi-group support, symlinks, hardlinks, permissions, and `fcntl()` locking. FAT16 compatibility layer. Real disk images that survive reboot — not a ramdisk that vanishes.

7. **Network Stack**: Full Ethernet→ARP→IPv4→ICMP→TCP implementation with real e1000 driver, interrupt-driven RX/TX, and barrier-safe MMIO. Ping both QEMU gateway and external IPs.

8. **Preemptive Multitasking**: Not cooperative scheduling — real PIT-driven preemption with process groups, signals, and fairness. Applications don't need to yield; the kernel takes back control.

---

## Release Notes: v1.7.0

**PCManFM-Qt Integration & QPA Stabilization**

This release marks the completion of a full graphical desktop environment with Qt6 support:

- **PCManFM-Qt File Manager**: Real upstream port of the Qt-based file manager; cross-compiled against PureUNIX's qt6 stack
- **Qt6 QPA Backend Finalization**: Fixed 4 separate repaint loop bugs (resize forwarding, pipe deadlock, fcntl O_NONBLOCK loss, pipe buffer too small)
- **PUDE Integration**: Window manager now stable for complex multi-window workflows
- **Real Hardware Validation**: All changes tested on both QEMU and HP Pavilion bare metal

See commits `a4000ea` (PCManFM-Qt port), `1e8febc` (libfm-qt), and `64c61dd` (MenuCache) for details.

<img width="640" height="428" alt="image" src="https://github.com/user-attachments/assets/2858314e-cb89-40e5-92ef-30a9047a90eb" />

---

## Features

### Core OS
- **Boot**: Multiboot1 and Multiboot2 compatible; boots through GRUB2 (`make iso`/`make run`) with a VBE linear-framebuffer request, with a Multiboot1 fallback for loaders (e.g. QEMU `-kernel`) that don't set up a graphics mode
- **Memory**: bitmap physical memory manager, identity-mapped paging (first 128 MiB), 8 MiB kernel heap with incremental `sbrk()` for growing heaps
- **Interrupts**: full IDT with 32 CPU exception handlers, 16 IRQ stubs, an `int $0x80` syscall gate, and a kernel-internal `int $0x81` process-termination trap
- **Protection**: ring 3 user-mode execution — a hardware TSS gives every task its own kernel stack, per-process page directories, and full per-process address space isolation
- **FPU/SSE**: Full x87 FPU and SSE2 context-save/restore on task switch via FXSAVE/FXRSTOR

### Drivers & Hardware
- **Console Subsystem**: 6 virtual terminals (Alt+F1-6), VGA text mode (80×25) with ANSI SGR colors, graphics-mode overlay support for SDL2/Qt6
- **Framebuffer**: VBE linear framebuffer with WC (write-combining) mapping for performance, boot splash, frame-accurate page-granular mapping for safe multi-page regions
- **Keyboard**: PS/2 with scan code set 1, full modifier support (shift/ctrl/alt/caps), arrow keys, function keys, proper mode-switching to/from graphics mode
- **Serial**: 16550 serial (COM1) with ANSI pass-through, mirrors all VGA output
- **Storage**: ATA PIO (primary master + slave, LBA28), real USB Mass Storage (BOT+SCSI) with persistent disk boot, SYNCHRONIZE CACHE writeback support
- **Network**: Real e1000 NIC driver with PCI probing, full Ethernet/ARP/IPv4/ICMP stack; ping working to both QEMU gateway and external IPs (e.g., 1.1.1.1)
- **USB**: Real xHCI host controller driver with USB2/USB3 SuperSpeed enumeration, USB Mass Storage SCSI, HID keyboard/mouse drivers; PS/2 keyboard fully supported for boot

### Filesystems & Storage
- **EXT2**: Read/write driver (primary root filesystem, ATA slave) with superblock, BGDT, inode table, direct + singly-indirect blocks, allocation/freeing, symlinks, hard links; multi-group support with proper num_groups accounting
- **FAT16**: Read/write driver (compatibility/testing store, ATA master) with 8.3 filenames, directory creation, file rename
- **VFS**: mount-table router with longest-prefix path resolution, symlink-following path resolution, Unix permission enforcement (`uid`/`gid`/mode), `fcntl()` locking for POSIX compliance (SQLite)
- **Persistent Storage**: Real GRUB bootloader on MBR disk images, hot-swap USB devices

### Process Management
- **Scheduler**: preemptive round-robin via PIT IRQ0, `fork()`/`exec()`/`wait()` syscalls with real process hierarchy, process groups, session IDs
- **Job Control**: Full ash shell job control (bg/fg/jobs/suspend), Ctrl+C/Ctrl+Z signal delivery, proper SIGTSTP/SIGCONT handling, `kill -N` by PID
- **PTY**: Real pseudo-terminal subsystem (kernel/pty.c with SYS_PTY_CREATE) enabling terminal emulation and multi-session support
- **Signals**: POSIX signal delivery (SIGTERM, SIGKILL, SIGSTOP, SIGCONT, SIGINT, SIGTSTP, SIGCHLD) with proper default handlers and disposition management

### Accounts & Security
- **Accounts**: `/etc/passwd` + `/etc/shadow` accounts; a first-boot wizard sets the root password and prints a setup guide, and every subsequent boot requires a `login:`/`Password:` prompt before the shell starts; `adduser`/`passwd` builtins manage accounts afterward (see `docs/users.md`)
- **CoreCrypto**: SHA-256, HMAC-SHA256, and PBKDF2-HMAC-SHA256 (10,000 iterations, random salt) back every password hash and login verification; self-tested at boot, printing `Crypto OK` (see `docs/crypto.md`)
- **IPC**: Real AF_UNIX domain sockets (SOCK_STREAM) for inter-process communication

### Userland & Applications
- **Shell**: ash (BusyBox) with 31+ builtins, interactive line editor with history, tab completion, pipes (`|`), I/O redirection (`<`, `>`, `>>`), background jobs, job control
- **Editor**: modal vim-like editor (`vim`/`vi`) with NORMAL, INSERT, COMMAND, and search modes, multi-level undo
- **Window Manager**: PUDE (PureUNIX Desktop Environment) — full graphical WM with window drag/resize, dock with pinned apps, app drawer, mouse support
- **File Manager**: PUFiles — graphical file manager with drag/drop, real directory operations (mkdir/rename/unlink/rmdir)
- **Syscalls**: 50+ syscalls via `int $0x80` including `fork`, `exec`, `wait`, `sbrk`, `flock`, `fcntl`, `ioctl`, `pty_create`, and many others
- **Graphics**: 16 MiB per-process graphics window, SDL2 library support, Qt6/QtWidgets support with real signal/slot mechanism and widget rendering

### Ported Applications
- **SQLite3** (`/bin/sqlite3`): Real upstream DBMS with persistent storage, POSIX fcntl locking, QEMU-verified with persistent disk
- **ncurses** (`/bin/ncdemo`): Real upstream terminal UI library with arrow/function-key support, SIGWINCH resizing
- **htop** (`/bin/htop`): System monitor showing real `/proc` data (comm, cmdline, CPU%, memory), F9 kill confirmed working
- **SDL2** (`/bin/sdltest.elf`): Graphics/events/timing/mouse all working, real hardware integration
- **Chocolate Doom** (`/bin/chocolate-doom`): Real upstream game engine, playable with mouse/keyboard, persistent saves, IWAD ships by default
- **libpng + zlib** (`/bin/imgview`): PNG image viewer with real scaling and color rendering
- **Qt6** (`libQt6Core.a`, `libQt6Gui.a`, `libQt6Widgets.a`): Full Qt6 with QMainWindow, QPushButton, QLabel, signals/slots, real event loop
- **PCManFM-Qt** (`/bin/pcmanfm-qt`): Graphical file manager (real upstream port)

### Networking
- **TCP/IP Stack**: Full Ethernet frame handling, ARP resolution, IPv4 routing, ICMP echo (ping), real IPv4 socket support in progress
- **e1000 Driver**: Real-hardware NIC driver with interrupt-driven RX/TX, proper memory barriers for MMIO descriptor rings

---

## Implementation Status

### Core Kernel (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| Multiboot boot | Complete | Multiboot1 + Multiboot2 headers; GRUB2 boot path with VBE framebuffer tag |
| GDT + TSS | Complete | Per-task TSS for ring 3 entry, dual stack switching |
| IDT + interrupt stubs | Complete | 32 exceptions + 16 IRQs + syscall gate (`0x80`) + termination trap (`0x81`) |
| PIC remapping | Complete | IRQ 0–15 → INT 32–47 |
| PIT (100 Hz timer) | Complete | PIT IRQ0 drives preemptive scheduler |
| Physical memory manager | Complete | Bitmap allocator, 128 MiB cap, real-hardware tested on HP Pavilion |
| Virtual memory / paging | Complete | Identity maps first 128 MiB; per-process page directories for full address-space isolation |
| Kernel heap | Complete | Linked-list allocator with split and coalesce; incremental sbrk() for user heaps |
| FPU/SSE context switch | Complete | FXSAVE/FXRSTOR on every task switch, enables Qt6 SIMD raster engine |

### Console & Display (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| Virtual terminals (6) | Complete | Alt+F1-6 switching, per-VT text/graphics mode state, real cooperative multi-console |
| VGA text driver | Complete | 80×25 mode, ANSI SGR colors, hardware cursor, scroll acceleration |
| VBE framebuffer | Complete | Linear framebuffer from Multiboot2 tag, WC mapping for performance, boot splash |
| Graphics mode | Complete | SDL2/Qt6/Chocolate Doom use 16 MiB per-process graphics window; proper return to text console |

### Input Devices (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| PS/2 keyboard | Complete | Scan code set 1, all modifiers, arrow/function keys, mode-aware key delivery |
| PS/2 mouse | Complete | Via xHCI HID, full 2D tracking for PUDE/PUFiles/Chocolate Doom |
| Serial | Complete | COM1, ANSI pass-through, console mirroring |

### Storage (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| ATA PIO driver | Complete | Primary master + slave, LBA28, sector read/write, barrier enforcement |
| xHCI USB host | Complete | Real hardware driver, USB2 + USB3 SuperSpeed enumeration, interrupt-driven |
| USB Mass Storage | Complete | BOT+SCSI protocol, SYNCHRONIZE CACHE, real persistent disk boot verified |
| FAT16 filesystem | Complete | Read/write, 8.3 names, dir creation, file rename, mounted at `/fat` |
| EXT2 filesystem | Complete | Read/write, symlinks, hardlinks, multi-group, direct+singly-indirect blocks (~268KiB files max), primary root at `/` |
| VFS layer | Complete | Mount-table routing, symlink resolution, Unix permissions, fcntl() locking |
| Persistent boot | Complete | Real MBR+GRUB+EXT2 disk images, hot-swap USB verified on HP Pavilion |

### Process Management (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| Per-process address space | Complete | Per-task page directory, full isolation, no shared kernel memory with other tasks (except via sockets) |
| Preemptive scheduler | Complete | PIT-driven context switch, round-robin per process group, fairness enforced |
| fork/exec/wait | Complete | Full process hierarchy, POSIX semantics, real userland-launched programs with proper stdio/stdout/stderr |
| Process groups & sessions | Complete | `fork()` inherits pgid, `setsid()` for login sessions, proper Ctrl+C/Z targeting |
| Virtual terminals | Complete | 6 independent virtual terminals with separate TTY, proper Ctrl+Z terminal suspension |
| PTY subsystem | Complete | Real pseudo-terminals (SYS_PTY_CREATE), ANSI terminal emulation, SIGWINCH on resize |
| Job control | Complete | ash `bg`/`fg`/`jobs` builtins, `%` job references, suspend/resume working verified |
| Signals | Complete | POSIX signals (SIGTERM, SIGKILL, SIGINT, SIGTSTP, SIGCONT, SIGCHLD), proper handlers and disposition |

### IPC (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| Pipes | Complete | FIFO queuing, up to 4 pipeline stages in shell, proper EOF propagation |
| AF_UNIX sockets | Complete | SOCK_STREAM domain sockets, real kernel implementation, tested and verified |
| fcntl() locking | Complete | POSIX record locking (F_SETLK/F_GETLK), enables SQLite persistence |

### Filesystems (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| Shell (ash) | Complete | Full interactive shell with 31+ builtins, history, tab completion, job control |
| vim editor | Complete | Modal edit (NORMAL/INSERT/COMMAND), multi-level undo, search, `:w`/`:q` |

### Syscalls (50+ implemented)
| Category | Syscalls |
|---|---|
| Process | `fork`, `exec`, `exit`, `wait`, `waitpid`, `getpid`, `getppid`, `getpgid`, `setpgid`, `setsid`, `kill`, `signal`, `sigaction`, `sigprocmask`, `pause`, `yield` |
| Memory | `sbrk`, `mmap`, `munmap` |
| I/O | `read`, `write`, `open`, `close`, `lseek`, `dup`, `dup2`, `pipe`, `fcntl`, `ioctl` |
| Files | `stat`, `lstat`, `fstat`, `access`, `chmod`, `chown`, `fchmod`, `fchown`, `readdir`, `chdir` |
| Filesystem | `mkdir`, `unlink`, `rmdir`, `rename`, `link`, `symlink`, `readlink`, `mount`, `umount` |
| Terminal | `pty_create`, `tcgetpgrp`, `tcsetpgrp` |
| Networking | `socket`, `bind`, `connect`, `listen`, `accept`, `sendto`, `recvfrom` |
| IPC | `poll`, `select` |
| Signals | `sigpending`, `sigsuspend` |
| Misc | `time`, `gettimeofday`, `reboot`, `shutdown`, `debug_setcred` (test-only) |

### Ported Applications (Complete)
| Application | Status | Notes |
|---|---|---|
| SQLite3 | Complete | Real upstream DBMS, persistent storage, QEMU-verified, fcntl() locking |
| ncurses | Complete | Real upstream TUI library, arrow/function keys, SIGWINCH, QEMU-verified |
| htop | Complete | System monitor, real `/proc` backend, kill (F9) verified working |
| SDL2 | Complete | Graphics/events/timing/mouse, QEMU-verified, Qt6 uses it for raster |
| Chocolate Doom | Complete | Real upstream game, playable, persistent saves, IWAD pre-shipped |
| zlib + libpng | Complete | PNG image support, imgview viewer, QEMU pixel-verified |
| Qt6 | Complete | QMainWindow, QPushButton, QLabel, signals/slots, full event loop |
| PCManFM-Qt | Complete | Graphical file manager, real upstream port, integrated into PUDE |
| BusyBox | Partial | grep/egrep/fgrep, real regex, priority-1 coreutils, Phase 4 complete |

### Networking (Complete)
| Subsystem | Status | Notes |
|---|---|---|
| e1000 NIC driver | Complete | PCI probing, interrupt-driven RX/TX, proper memory barriers |
| Ethernet | Complete | Frame handling, VLAN awareness, real hardware tested |
| ARP | Complete | Address resolution, dynamic cache |
| IPv4 | Complete | Packet assembly, routing, ICMP |
| ICMP | Complete | Echo request/reply (ping), tested to external IPs (1.1.1.1) |
| TCP/IP sockets | In progress | Socket API working, data transfer tested |

---

## Directory Layout

```
PureUNIX/
├── arch/i386/           GDT+TSS, IDT, PIC, PIT, syscall, context switch, ring3 entry, FPU/SSE
├── boot/                Multiboot entry stub, linker script, GRUB config
├── drivers/             Console subsystem (6 VTs), VGA, VBE framebuffer, boot splash
│   ├── keyboards/       PS/2 keyboard, USB HID
│   ├── serial.c         16550 UART
│   ├── ata.c            ATA PIO disk (master + slave, LBA28)
│   ├── e1000.c          Real e1000 NIC driver (PCI, interrupt-driven)
│   ├── xhci.c           Real xHCI USB host controller (USB2 + USB3)
│   └── usb_msd.c        USB Mass Storage (BOT+SCSI)
├── kernel/              Main, PMM, VMM, per-process page directories, heap
│   ├── elf.c            ELF32 loader
│   ├── task.c           Process management, fork/exec/wait, preemptive scheduler
│   ├── signal.c         POSIX signal delivery and handlers
│   ├── pty.c            Pseudo-terminal subsystem (SYS_PTY_CREATE)
│   ├── flock.c          fcntl() locking for file persistence
│   ├── net/             Network stack (e1000, Ethernet, ARP, IPv4, ICMP, TCP)
│   └── vt.c             Virtual terminal management (6 VTs)
├── fs/                  Filesystem layer
│   ├── ext2/            EXT2 read/write driver (superblock, BGDT, inode, dir, file, multi-group)
│   ├── fat16.c          FAT16 read/write driver (8.3 names, dir operations)
│   └── vfs.c            Mount-table VFS: path resolution, symlinks, permissions, fcntl() locking
├── editor/              In-kernel vim-like modal editor (modal, undo, search, :w/:q)
├── shell/               ash (BusyBox) shell integration
│   ├── parser.c         Command parsing, pipes, redirection
│   ├── line_editor.c    History, completion, line editing
│   └── builtins/        31+ builtins (cd, ls, cat, cp, mv, rm, mkdir, ps, kill, jobs, fg, bg, etc.)
├── user/                Userland programs and libraries
│   ├── crt0.S           C runtime startup (handles signals, calls main(), invokes exit via int $0x81)
│   ├── libpure/         Syscall wrappers and standard library
│   ├── hello.c          Simple hello world test
│   ├── calc.c           Calculator
│   ├── systest.c        363-case syscall/filesystem regression suite
│   ├── pude/            PUDE window manager (graphical multi-window, dock, app drawer, mouse)
│   ├── pude_gfx.h       Graphics primitives (clipped text, shapes)
│   ├── pude_widgets.h   Widget library (buttons, text, layout)
│   └── pude_launch.c    Fork/exec handoff for launching apps
├── third_party/         Vendored/ported applications
│   ├── newlib/          C library (BSD signal numbering)
│   ├── tcc/             Tiny C compiler
│   ├── lua/             Lua interpreter
│   ├── sqlite/          SQLite3 DBMS
│   ├── ncurses/         ncurses TUI library with real terminfo
│   ├── zlib/            zlib compression
│   ├── libpng/          PNG image library
│   ├── sdl2/            SDL2 graphics library (video, events, timing)
│   ├── qt6/             Qt6 framework (Core, Gui, Widgets)
│   ├── libfm-qt/        PCManFM Qt library
│   ├── pcmanfm-qt/      PCManFM-Qt file manager
│   ├── chocolate-doom/  Doom game engine (upstream)
│   └── busybox/         BusyBox utilities (Phase 4: regex, grep/egrep/fgrep, coreutils)
├── tools/               Build and testing utilities
│   ├── mkfat16.py       FAT16 image builder
│   ├── mkext2.py        EXT2 image builder (multi-group)
│   ├── vt-inject-test.py QEMU VT testing via HMP monitor
│   └── test-pude/*.py   PUDE regression test suite
├── libc/                Freestanding C library (string, printf, stdlib, ctype)
├── include/pureunix/    Public headers (syscalls, types, stdio, stdlib)
├── build/               Build output (generated)
├── docs/                User documentation (users.md, syscalls.md, crypto.md, gdb.md)
└── Makefile             Build orchestration
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

## Real Hardware Support

PureUNIX boots and runs on real x86-32 hardware via BIOS/UEFI legacy mode. Tested on:

- **HP Pavilion (real bare metal)**: Full boot with e1000 NIC, xHCI USB, EXT2 persistence
  - Boots via GRUB2 on real MBR disk
  - Persistent login sessions and file storage
  - Interactive shell with job control
  - Network stack ping verified
  - Multiple virtual terminals with Alt+F1-6
  - GPU framebuffer support (VBE)

To boot on real hardware:

1. Create a USB stick from the ISO: `dd if=build/pureunix.iso of=/dev/sdX bs=4M`
2. Insert USB stick and boot (may need to enable legacy mode in BIOS)
3. GRUB2 bootloader loads kernel and EXT2 root filesystem
4. Login with root password set during first boot

**Note**: Real hardware may have different hardware drivers, video modes, or boot quirks. Kernel panic diagnostics are serial console (`COM1` output). See `docs/gdb.md` for GDB debugging over serial.

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

The very first boot on a disk image runs a setup wizard that asks you to set a root password and prints a short setup guide; every boot after that (this one included) shows a `login:`/`Password:` prompt before the shell starts. See `docs/users.md`.

---

## First Commands

### Basic Shell
```sh
help                        # show all builtins
ls /                        # EXT2 root filesystem
cat /README.TXT             # reads from EXT2
pwd                         # print working directory
cd /bin && ls               # change directory, list installed programs
whoami                      # show logged-in user
cat /etc/passwd             # view user accounts
```

### User Management
```sh
adduser alice               # root only — creates new user with home directory
passwd                      # change password for current user
passwd alice                # root only — change another user's password
su alice                    # switch user (requires password)
```

### Filesystem & Editing
```sh
touch note.txt              # create empty file
echo hello > note.txt       # redirect text to file
cat note.txt                # read file
mkdir docs                  # create directory
vim docs/todo.txt           # edit file in vim (modal editor)
cp note.txt docs/           # copy file
mv note.txt docs/backup.txt # move/rename file
rm docs/backup.txt          # delete file
```

### Graphics & GUI (PUDE Desktop)
```sh
pude                        # launch PUDE graphical desktop environment
# Once in PUDE, use mouse to open windows, drag/resize windows, launch apps from dock/drawer
```

### Applications & Games
```sh
sqlite3                     # SQLite3 database shell (interactive)
htop                        # system monitor with real-time process stats
ncdemo                      # ncurses demo (arrow keys, colors, terminal UI)
chocolate-doom              # Play Doom 1 (mouse/keyboard, persistent saves)
imgview /path/to/image.png  # View PNG images with scaling
pcmanfm-qt                  # Graphical file manager (PCManFM-Qt port)
sdltest.elf                 # SDL2 test (graphics, events, mouse)
```

### Testing & Diagnostics
```sh
systest                     # 363-case syscall/filesystem regression suite
ext2test                    # EXT2-specific filesystem tests
hello                       # simple hello-world program
ps                          # list all running processes with job control info
kill -TERM 1234             # send signal to process (SIGTERM)
jobs                        # show background jobs
bg %1                       # resume job in background
fg %1                       # bring job to foreground
```

### System Management
```sh
free                        # show free memory
mount                       # show mounted filesystems (/ and /fat)
df                          # disk usage
reboot                      # reboot system
shutdown                    # halt system
```

### Network
```sh
ping 10.0.2.2               # ping QEMU gateway
ping 1.1.1.1                # ping external IP (requires internet gateway)
```

### Job Control & Multi-tasking
```sh
hello &                     # run program in background
Ctrl+Z                      # suspend running program (SIGTSTP)
bg                          # resume suspended job in background
fg %1                       # bring job to foreground
jobs -l                     # list jobs with PIDs
kill -9 %1                  # kill job by number
```

---

## Using the Graphical Environment (PUDE)

### Starting PUDE
From the shell prompt:
```sh
pude
```

PUDE is a complete graphical window manager that runs in graphics mode (1024×768 or VBE native mode). It features:

- **Dock** (bottom-center): 4 pinned application icons (htop, pcmanfm-qt, sqlite3, chocolate-doom)
- **App Drawer** (top-right grid icon): Full list of graphical applications
- **Window Management**: Click title bar to drag, bottom-right corner to resize, close button to exit
- **Mouse Support**: Full 2D mouse tracking via PS/2 or USB HID
- **Multi-App**: Launch and run multiple graphical apps simultaneously (e.g., file manager + text editor + game)

### Launching Applications from PUDE
1. Click the app drawer icon (grid) in the top-right corner
2. Click any application icon to launch it
3. Use the mouse to interact with windows

### PUDE to Shell Handoff
- Press **Escape** or click close (X) to return to the shell
- Windows opened from PUDE run as child processes of the PUDE manager
- Closing PUDE exits all child graphical apps

### Examples
```
# From shell:
pude                                # Launch PUDE desktop
# (now in graphics mode with mouse cursor)
# Click app drawer → click htop → system monitor launches in a window
# Drag window around by title bar
# Click close button to exit htop, return to PUDE
# Press Escape to exit PUDE and return to shell
```

---

## Debugging & Development

### Serial Console Debugging
All kernel output goes to serial (COM1). To see real-time kernel messages:

```sh
# In QEMU, check the serial output:
make run 2>&1 | tee qemu-output.log
```

### GDB Debugging
See `docs/gdb.md` for step-by-step GDB setup over QEMU serial:
```sh
# Terminal 1: start QEMU in GDB mode
qemu-system-i386 -S -gdb tcp::1234 -m 128M -cdrom build/pureunix.iso ...

# Terminal 2: start GDB
i686-elf-gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) continue
```

### Testing Regression Suites
```sh
systest                     # 363-case syscall/filesystem regression suite
                            # Verifies fork, exec, pipes, signals, file I/O, EXT2, etc.
```

All regression suites must pass before committing changes that touch kernel core. QEMU with persistent disk must show 343/345 baseline failures (only 2 unrelated console-geometry checks fail).

---

## Architecture & Design

### Kernel Design Philosophy
- **Minimal & Understandable**: Single kernel developer can hold entire system in mind; no thousand-line functions or tangled subsystem interactions
- **POSIX Compatible**: Real `fork()`/`exec()`/`wait()`, signals, PTY, file locking — standard UNIX applications run without modification
- **Preemptive Multitasking**: PIT IRQ0 drives context switches; no application can starve others
- **Hardware Abstraction**: Drivers isolated in `drivers/` layer; kernel core doesn't know about specific hardware beyond basic CPU/MMU/PIC/PIT

### Memory Model
- **Identity-Mapped Kernel**: First 128 MiB of physical memory directly mapped to kernel space
- **Per-Process Address Space**: Each task gets its own page directory; user space is isolated per process, not shared
- **ELF Window**: Executable loads at fixed VA `0x400000` in user space; separate for each task
- **Heap Growth**: `sbrk()` dynamically grows heap up to task's configured limit (typically 64+ MiB for modern apps like Qt6)

### Scheduler
- **Preemptive Round-Robin**: PIT ticks every 10ms (100 Hz); context switch happens on every tick if a higher-priority process or different process group exists
- **Process Groups**: `fork()` and `setsid()` manage process groups for job control; Ctrl+C/Z targets all processes in the group
- **Fairness**: Each process gets a time slice equal to other processes at the same priority (SCHED_RR)

### Syscall ABI
- EAX = syscall number (SYS_*)
- EBX, ECX, EDX, ESI, EDI, EBP = arguments (up to 6 args per syscall)
- Return value in EAX; error in ECX (non-zero = errno)
- `libpure` wrapper functions (e.g., `fork()`, `write()`) translate from C to this ABI

---

## Current Limitations

### Design Constraints
- **Physical memory cap**: 128 MiB identity-mapped; no demand paging or physical address space beyond this
- **Per-process graphics window**: Only 16 MiB allocated for SDL2/Qt6 framebuffer; games/graphical apps are limited to 16 MiB heap
- **Indirect blocks**: EXT2 supports direct + singly-indirect blocks only (≈ 268 KiB per file); no doubly/triply-indirect
- **Preemption edge cases**: Preemptive scheduling is implemented but real-time signals and priorities are not; kernel-internal async routines may be incomplete

### Filesystem
- **FAT16 8.3 names**: Long filenames not supported; all files restricted to 8.3 DOS format
- **FAT16 rename**: Cannot move entries between directories (rename within same dir only)
- **No LFN**: VFAT long filename extensions not implemented

### Terminal & Input
- **QEMU USB keyboard limitation**: PS/2 keyboard and xHCI USB HID both work, but QEMU's `-serial stdio` has no usable stdin; use `vt-inject-test.py` with HMP socket to inject keys into headless QEMU
- **VT resize delay**: PTY clients see SIGWINCH after resize but ncurses may lag one paint cycle before redrawing

### Networking
- **No DNS**: IPv4 stack works for direct IP; no DNS resolution via nameservers
- **No DHCP**: Static IP configuration required (manually set via socket APIs)
- **TCP data transfer slow**: TCP/IP implemented but not optimized; single-threaded event loop
- **No multicast/UDP broadcast**: Unicast only

### Graphics & GUI
- **No hardware acceleration**: All rendering single-threaded on CPU (Qt6 uses SDL2 software raster)
- **PUDE dock limited**: 4 pinned app slots; app drawer is a static list, not dynamic
- **No drag-drop between apps**: Clipboard/IPC works only between PUDE and launched apps
- **Mouse cursor lag**: Rendering architecture has a single-frame latency; investigated (region theory disproved), fix designed but not committed

### Applications
- **Chocolate Doom mods**: Only IWAD (doom1.wad) supported; no WAD file loading from user disk
- **PCManFM-Qt**: Ported but not yet integrated into PUDE's app launcher; must run from shell
- **BusyBox phase 4 only**: Full coreutils suite not ported; priority-1 utilities (grep, ls, cat, etc.) done; man pages and localization missing
- **No floating-point printf**: `%f` not supported in kernel printf or newlib's snprintf (long-double still missing even after --enable-newlib-io-long-long)

### Syscalls & POSIX Compliance
- **SYS_EXIT behavior**: `exit(N)` sets process exit code but doesn't actually terminate; real termination happens via `int $0x81` trap in crt0
- **No poll/select parity**: `poll()` exists but `select()` is minimal; epoll not implemented
- **No mmap for devices**: `mmap()` works for regular files only, not /dev/mem or /dev/zero
- **No splice/vmsplice**: Zero-copy pipe operations not supported
- **POSIX ACLs not supported**: Traditional rwx permissions only, no extended attributes
- **No sysctl**: Kernel tunables not user-accessible

### Debugging & Tracing
- **No strace**: Syscall tracing not implemented (GDB works, see `docs/gdb.md`)
- **Page fault panic**: Any segfault in user code panics the kernel; no graceful task termination
- **No kernel logging levels**: All kernel output goes to serial/VGA; no syslog daemon

### Performance
- **Single-threaded kernel**: No kernel-level concurrency; syscalls block all other tasks
- **Small pipe buffer**: 256 KiB limit per pipe (fixed from 4 KiB after Qt6 port)
- **No I/O scheduling**: ATA reads are blocking; no elevator algorithm or read-ahead

---

## Completed Milestones (v1.7.0)

- ✅ Multiboot boot (Multiboot1 + Multiboot2)
- ✅ Per-process address space (separate page directories, full isolation)
- ✅ Preemptive scheduling via PIT IRQ0
- ✅ `fork()`/`exec()`/`wait()` with full process hierarchy
- ✅ Job control (Ctrl+Z, bg/fg, `kill`, job references)
- ✅ Virtual terminals (6 independent VTs with Alt+F1-6)
- ✅ POSIX signals (SIGTERM, SIGKILL, SIGINT, SIGTSTP, SIGCONT, SIGCHLD)
- ✅ Preemptive scheduler (PIT-driven, process groups, sessions)
- ✅ Real PTY subsystem (pseudo-terminals, ANSI emulation, SIGWINCH)
- ✅ Real e1000 NIC driver (Ethernet, ARP, IPv4, ICMP, TCP)
- ✅ USB stack (xHCI, USB2/USB3, Mass Storage, HID keyboard/mouse)
- ✅ FPU/SSE context switch (FXSAVE/FXRSTOR)
- ✅ PUDE window manager (graphical multi-window, dock, app drawer, mouse)
- ✅ Graphical file manager (PUFiles, real directory ops)
- ✅ SQLite3 (persistent storage with fcntl locking)
- ✅ ncurses (full TUI library with terminfo)
- ✅ htop (system monitor with `/proc` backend)
- ✅ SDL2 (graphics, events, timing, mouse)
- ✅ Chocolate Doom (playable game engine)
- ✅ Qt6 (QMainWindow, widgets, signals/slots, real event loop)
- ✅ PCManFM-Qt (graphical file manager)
- ✅ Real GRUB bootloader on persistent MBR disk images
- ✅ Dynamic incremental `sbrk()` (heap grows as needed)
- ✅ `fcntl()` locking (POSIX file record locking)
- ✅ AF_UNIX sockets (SOCK_STREAM domain sockets)
- ✅ BusyBox Phase 4 (grep/egrep/fgrep, real regex, priority-1 coreutils)

## Future Roadmap

### High Priority (Post-v1.7.0)
- Page fault handler for graceful task termination (don't panic on segfault)
- Finish BusyBox Phase 5+ (full coreutils, man pages, localization)
- Fix M10 bug: Ctrl+Z on real foreground job sometimes hangs the shell
- PUDE app drawer integration for PCManFM-Qt, SQLite3 GUI
- Multi-session TTY support (telnet daemon, virtual serial ports)
- Fix or remove the PUDE cursor-lag rendering issue (damage rectangle optimization)

### Medium Priority
- LFN (long filename) support in FAT16 driver
- Doubly-indirect and triply-indirect block support in EXT2
- DHCP client for automatic IP configuration
- DNS client for hostname resolution
- TCP data transfer optimization (buffering, window scaling)
- Demand paging (lazy page allocation instead of eager BSS)
- Kernel-level thread support (pthreads library)
- Real hardware (x86_64 port for modern systems; currently i386 only)

### Lower Priority / Known Tradeoffs
- `mmap()` device support (/dev/mem, /dev/zero) — security risk if exposed
- Kernel preemption (currently only user-mode preemption) — adds complexity
- Floating-point printf (`%f`, `%e`, `%g`) — rarely used in systems programming
- Splicing / zero-copy syscalls — nice-to-have for performance
- POSIX ACLs — complex, rarely used on embedded systems
- systemd or init daemon — PureUNIX uses simple multi-console login model
- WiFi driver (would require 802.11 stack) — not planned, wired Ethernet only
