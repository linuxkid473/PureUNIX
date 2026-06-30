# Build

## Toolchain Requirements

| Tool | Purpose |
|---|---|
| `i686-elf-gcc` | Cross-compiler for kernel and user programs |
| `i686-elf-gcc` (as assembler) | Assembles `.S` files |
| `make` | Build orchestration |
| `python3` | Generates the FAT16 disk image |
| `qemu-system-i386` | Emulator for `make run` |

Optional:

| Tool | Purpose |
|---|---|
| `grub-mkrescue` | Builds a bootable ISO (`make iso`) |
| `xorriso` | Required by `grub-mkrescue` internally |

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
1. All kernel object files from `kernel/`, `arch/`, `drivers/`, `fs/`, `libc/`, `shell/`, `editor/`.
2. All assembly object files from `boot/` and `arch/`.
3. Links `build/pureunix.elf` using `boot/linker.ld`.
4. Compiles and links five user ELF programs: `hello`, `calc`, `viewer`, `editor`, `sh`.
5. Runs `tools/mkfat16.py` to produce `build/pureunix.img`.

### `make run`

Requires `build/pureunix.elf` and `build/pureunix.img`. Runs:

```sh
qemu-system-i386 \
    -m 128M \
    -kernel build/pureunix.elf \
    -drive file=build/pureunix.img,format=raw,if=ide,index=0 \
    -serial stdio \
    -no-reboot \
    -no-shutdown
```

Key QEMU options:
- `-kernel`: uses QEMU's built-in Multiboot loader (does not go through GRUB).
- `-m 128M`: provides exactly 128 MiB of RAM, matching the PMM cap.
- `if=ide,index=0`: attaches the disk image as ATA primary master.
- `-serial stdio`: routes COM1 output to the host terminal.
- `-no-reboot -no-shutdown`: keeps the QEMU window open after a shutdown/reboot so output can be reviewed.

### `make iso`

Requires `grub-mkrescue` on `PATH`. Builds a bootable ISO at `build/pureunix.iso`:

1. Creates `build/iso/boot/grub/` and copies `boot/grub.cfg`.
2. Copies `build/pureunix.elf` to `build/iso/boot/pureunix.elf`.
3. Runs `grub-mkrescue -o build/pureunix.iso build/iso`.

The ISO uses GRUB's Multiboot2 loader. PureUnix embeds both Multiboot1 and Multiboot2 headers; GRUB selects Multiboot2.

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

## Boot Configuration (ISO only)

`boot/grub.cfg`:

```
set timeout=0
set default=0
menuentry "PureUnix" {
    multiboot2 /boot/pureunix.elf
    boot
}
```

`timeout=0` boots immediately. Only GRUB's Multiboot2 command is used; the kernel is not passed any module.

---

## GRUB vs. QEMU Direct Loading

| Mode | Loader | Protocol used |
|---|---|---|
| `make run` | QEMU `-kernel` | Multiboot1 |
| `make iso` | GRUB 2 | Multiboot2 |

The kernel's `_start` (`boot/multiboot2.S`) detects which protocol was used by checking the magic value in `EAX`:

- `0x2BADB002`: Multiboot1 (QEMU direct)
- `0x36D76289`: Multiboot2 (GRUB)

Both paths push `EAX` (magic) and `EBX` (MBI address) before calling `kernel_main`. The PMM reads Multiboot memory maps from both formats.
