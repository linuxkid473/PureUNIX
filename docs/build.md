# Build

## Toolchain Requirements

| Tool | Purpose |
|---|---|
| `i686-elf-gcc` | Cross-compiler for kernel and user programs |
| `i686-elf-gcc` (as assembler) | Assembles `.S` files |
| `make` | Build orchestration |
| `python3` | Generates the FAT16 and EXT2 disk images |
| `grub-mkrescue` (`i686-elf-grub-mkrescue`) | Builds the bootable ISO both `make iso` and `make run` boot from |
| `xorriso` | Required by `grub-mkrescue` internally |
| `mtools` | Required by `grub-mkrescue` internally (builds the El Torito FAT image) |
| `qemu-system-i386` | Emulator for `make run` |

`grub-mkrescue`/`xorriso`/`mtools` are no longer optional — since both `make run` and `make iso` boot the same GRUB-built ISO (see "GRUB vs. QEMU Direct Loading" below), they're required for any interactive testing, not just `make iso` on its own. On macOS: `brew install i686-elf-grub xorriso mtools`.

The cross-compiler prefix defaults to `i686-elf-` and can be overridden:

```sh
make CROSS=i686-elf-
```

---

## Compiler Flags

### Kernel flags (`KERNEL_CFLAGS`)

```
-std=gnu99
-ffreestanding        # no hosted-environment assumptions
-O2
-Wall -Wextra -Wno-unused-parameter
-fno-stack-protector  # no SSP; no __stack_chk_fail needed
-fno-pic -fno-pie     # no position-independent code
-m32 -march=i686      # 32-bit IA-32
-Iinclude             # kernel headers
-D__PUREUNIX_KERNEL__ # guards kernel-only declarations
```

### User flags (`USER_CFLAGS`)

Same as kernel flags, plus:

```
-fno-builtin          # prevents compiler from substituting calls to libc builtins
```

User code does not define `__PUREUNIX_KERNEL__`, so kernel-internal declarations are hidden.

### Linker flags

Kernel:
```
-T boot/linker.ld
-ffreestanding -O2 -nostdlib
-Wl,--build-id=none   # suppress build ID section
-lgcc                 # compiler support library (soft divide, etc.)
```

User:
```
-T user/linker.ld
-ffreestanding -nostdlib
-Wl,--build-id=none
-lgcc
```

---

## Build Targets

### `make` (default: `all`)

Builds:
1. All kernel object files from `kernel/`, `arch/`, `drivers/`, `fs/`, `libc/`, `shell/`, `editor/`, `net/`.
2. All assembly object files from `boot/` and `arch/`.
3. Links `build/pureunix.elf` using `boot/linker.ld`.
4. Compiles and links every user ELF program (libpure- and newlib-linked, plus `neatvi`, BusyBox, and TinyCC — see `USER_PROGRAMS`/`NEWLIB_PROGRAMS` in the Makefile).
5. Runs `tools/mkfat16.py` to produce `build/pureunix.img` (the FAT16 image) and `tools/mkext2.py` to produce `build/ext2.img` (the EXT2 root filesystem, including `/docs`, BusyBox, and the TinyCC sysroot).

### `make iso`

Requires `grub-mkrescue` on `PATH`. Builds a **standalone, self-contained** bootable ISO at `build/pureunix.iso` that boots into the exact same environment as `make run` — no separate disk files need to be attached:

1. Builds `build/pureunix.elf`, `build/pureunix.img` (FAT16), and `build/ext2.img` (EXT2) if not already up to date.
2. Creates `build/iso/boot/grub/` and copies `boot/grub.cfg`.
3. Copies `build/pureunix.elf` → `build/iso/boot/pureunix.elf`, `build/pureunix.img` → `build/iso/boot/fat.img`, `build/ext2.img` → `build/iso/boot/root.img`.
4. Runs `grub-mkrescue -o build/pureunix.iso build/iso`.

The ISO uses GRUB's Multiboot2 loader. PureUnix embeds both Multiboot1 and Multiboot2 headers; GRUB selects Multiboot2. `fat.img` and `root.img` travel as Multiboot2 **modules** (GRUB's `module2` command, see `boot/grub.cfg`) — GRUB loads them into RAM alongside the kernel, and `kernel_main()` mounts them directly from memory via a RAM-backed `disk_device_t` (`drivers/ramdisk.c`) instead of real ATA hardware. See "Boot Configuration" and `docs/boot.md` for how the kernel finds and reserves this memory.

`grub-mkrescue` produces a **hybrid** image (El Torito BIOS boot catalog + a protective MBR from GRUB's `boot_hybrid.img`), so the same `build/pureunix.iso` can be:
- attached to QEMU with `-cdrom`,
- mounted as a virtual CD/DVD in VirtualBox or VMware, or
- written directly to a USB stick (`dd if=build/pureunix.iso of=/dev/sdX`, or Rufus/BalenaEtcher on Windows/macOS) and booted on real BIOS hardware.

Networking (the e1000 NIC) and the framebuffer bootsplash are kernel/driver features, not part of the ISO — the *hypervisor* still needs a virtual e1000 NIC attached for `ip`/`arp`/`icmp` to have anything to talk to (`make run` passes `-netdev user,id=net0 -device e1000,netdev=net0`; VirtualBox/VMware users should attach an "Intel PRO/1000" NIC the same way). On real hardware, PureUnix's driver only supports the same `8086:100e` (e1000) chipset that this port targets, matching QEMU's emulated device.

### `make run`

Requires `build/pureunix.iso` (built automatically as a dependency). Runs:

```sh
qemu-system-i386 -m 128M -cdrom build/pureunix.iso -boot d \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -serial stdio -no-reboot -no-shutdown
```

Key QEMU options:
- `-cdrom ... -boot d`: boots the same standalone ISO `make iso` produces — through real GRUB, not QEMU's built-in Multiboot1 `-kernel` loader (which never sets up a graphics mode, so the multiboot2 framebuffer tag in `boot/multiboot2.S` would go unanswered).
- `-m 128M`: provides exactly 128 MiB of RAM, matching the PMM cap (`MAX_MEMORY_BYTES`).
- `-netdev user,id=net0 -device e1000,netdev=net0`: attaches a virtual e1000 NIC on QEMU's user-mode (SLIRP) network, matching the static IP `kernel_main()` configures (`10.0.2.15/24` via `10.0.2.2`).
- `-serial stdio`: routes COM1 output to the host terminal (output only — PureUnix's keyboard driver is PS/2-only, so serial `stdin` is not read; see `docs/architecture.md`).
- `-no-reboot -no-shutdown`: keeps the QEMU window open after a shutdown/reboot so output can be reviewed.

`make run` and `make iso` share the exact same boot image, so there is no duplicated boot logic to keep in sync between them.

### `make clean`

Removes the entire `build/` directory.

---

## Object and Dependency Generation

Source files are discovered with `find`:

```makefile
KERNEL_C_SRCS  := $(shell find kernel arch drivers fs libc shell editor -name '*.c' | sort)
KERNEL_AS_SRCS := $(shell find boot arch -name '*.S' | sort)
```

Each source file maps to a corresponding object under `build/`:

```
kernel/main.c           → build/kernel/main.o
arch/i386/gdt.c         → build/arch/i386/gdt.o
boot/multiboot2.S       → build/boot/multiboot2.o
```

Dependency files (`.d`) are generated with `-MMD -MP` and included at the bottom of the Makefile. Editing a header triggers recompilation of all `.c` files that include it.

---

## User Program Build Process

Each user ELF is built from three object files:

```
build/user/crt0.o       (from user/crt0.S)
build/user/libpure.o    (from user/libpure.c)
build/user/<prog>.o     (from user/<prog>.c)
```

Linked with `user/linker.ld` (base address `0x400000`).

The resulting ELFs are placed under `build/user/` and then consumed by `mkfat16.py`:

```sh
python3 tools/mkfat16.py build/pureunix.img \
    build/user/hello.elf \
    build/user/calc.elf \
    build/user/viewer.elf \
    build/user/editor.elf \
    build/user/sh.elf
```

---

## Disk Image Generation

`tools/mkfat16.py` creates `build/pureunix.img` (32 MiB FAT16):

1. Writes a BPB boot sector with OEM string `PUREUNIX`.
2. Initializes two FAT copies.
3. Creates a root directory with a 512-entry capacity.
4. Creates `/README.TXT`.
5. Creates `/BIN/` directory.
6. Writes each passed `.elf` file under `/BIN/` with an uppercase 8.3 name.

The image is a flat raw binary, suitable for direct attachment as an IDE drive.

---

## Boot Configuration

`boot/grub.cfg`:

```
set timeout=1
set default=0

serial --unit=0 --speed=115200
terminal_output console serial

menuentry "PureUnix" {
    multiboot2 /boot/pureunix.elf
    module2 /boot/fat.img fat.img
    module2 /boot/root.img root.img
    boot
}
```

`timeout=1` boots automatically after one second. `module2 <path> <cmdline>` tells GRUB to load a file into RAM alongside the kernel and record its physical address range plus the trailing `cmdline` string (`fat.img`/`root.img`) in the Multiboot2 info structure. `kernel_main()` (`kernel/main.c`) looks up modules by that `cmdline` string via `pmm_module_get()` and wraps each one as a `disk_device_t` with `ramdisk_attach()` (`drivers/ramdisk.c`), then mounts FAT16/EXT2 on it exactly as it would a real ATA disk — see `docs/boot.md` and `docs/memory.md` for how `kernel/pmm.c` reserves each module's physical memory (so the frame allocator and kernel heap never overwrite it) and `kernel/vmm.c`'s 128 MiB identity map for how the kernel accesses a module's bytes directly by physical address.

If no module with a matching `cmdline` is found (e.g. a from-source `-kernel` boot with no GRUB involved at all), `kernel_main()` falls back to real ATA disks (`ata_primary_master()`/`ata_primary_slave()`) — the driver in `drivers/ata.c` is unchanged and still available for real IDE hardware or hand-built QEMU invocations that attach `-drive ...,if=ide`.

---

## GRUB vs. QEMU Direct Loading

| Mode | Loader | Protocol used |
|---|---|---|
| `make run` / `make iso` | GRUB 2 (`grub-mkrescue`) | Multiboot2 |
| Hand-built `qemu-system-i386 -kernel build/pureunix.elf ...` | QEMU's built-in loader | Multiboot1 |

Both `make run` and `make iso` boot through `build/pureunix.iso` via real GRUB — QEMU's built-in `-kernel` flag is not used by either target, since it never sets up a graphics mode. It remains available for ad hoc debugging, without disk images or networking.

The kernel's `_start` (`boot/multiboot2.S`) detects which protocol was used by checking the magic value in `EAX`:

- `0x2BADB002`: Multiboot1 (QEMU direct)
- `0x36D76289`: Multiboot2 (GRUB)

Both paths push `EAX` (magic) and `EBX` (MBI address) before calling `kernel_main`. The PMM reads Multiboot memory maps from both formats; module (`module2`) tags are Multiboot2-only, so a plain Multiboot1 `-kernel` boot never has any modules and always falls back to ATA disks.
