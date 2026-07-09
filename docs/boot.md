# Boot Process

## Multiboot Headers

`boot/multiboot2.S` embeds **two** Multiboot headers back to back in a dedicated `.multiboot` section that the linker places first in the image.

**Multiboot1 header** (`.multiboot` section, 4-byte aligned):
```
Magic:    0x1BADB002
Flags:    0x00000003  (align modules to 4 KiB; include memory info)
Checksum: -(magic + flags)
```
This allows QEMU's `-kernel` flag and Multiboot1-only boot loaders to load the kernel.

**Multiboot2 header** (`.multiboot2` section, 8-byte aligned):
```
Magic:    0xE85250D6
Arch:     0  (i386 protected mode)
Length:   (mboot2_end - mboot2_start)
Checksum: -(magic + arch + length)
End tag:  type=0, flags=0, size=8
```
This allows GRUB 2 and other Multiboot2 loaders.

The linker script keeps both sections via `KEEP(*(.multiboot)) KEEP(*(.multiboot2))`.

---

## Boot Stack

Declared in the `.bss` section in `multiboot2.S`:

```asm
boot_stack_bottom:
    .skip 32768          ; 32 KiB
boot_stack_top:
```

The stack grows downward from `boot_stack_top`. This is the stack used by `kernel_main` and every function called from it until tasking replaces the active stack pointer via `context_switch`.

---

## `_start` Entry Point

The boot loader enters the kernel at `_start` in 32-bit protected mode with interrupts disabled. The Multiboot specification places the Multiboot magic value in `EAX` and a pointer to the Multiboot information structure in `EBX`.

```asm
_start:
    cli
    mov $boot_stack_top, %esp   ; install stack
    xor %ebp, %ebp              ; terminate stack frames
    push %ebx                   ; arg2: mbi_addr
    push %eax                   ; arg1: magic
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang
```

If `kernel_main` ever returns (it does not in normal operation), the CPU halts in a permanent loop.

---

## Linker Script

`boot/linker.ld` controls the kernel's memory layout:

| Section | Address | Alignment | Content |
|---|---|---|---|
| Entry | — | — | `_start` |
| Load address | `1M` | — | Kernel base |
| `.multiboot` | 1 MiB | 8 | Multiboot1 + Multiboot2 headers |
| `.text` | next | 4 KiB | All code |
| `.rodata` | next | 4 KiB | Read-only data, `.eh_frame` |
| `.data` | next | 4 KiB | Initialized globals |
| `.bss` | next | 4 KiB | COMMON + zero-initialized (includes boot stack) |
| `/DISCARD/` | — | — | `.comment`, `.note` (stripped) |

The linker exports `__kernel_start` and `__kernel_end` symbols that the PMM and heap use to reserve and locate kernel memory.

---

## GRUB Configuration

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

Both `make iso` and `make run` boot through this GRUB config — `make run` boots `build/pureunix.iso` via QEMU's `-cdrom`, the same image `make iso` produces; it does not use QEMU's `-kernel` flag. See `docs/build.md` for the Makefile side of ISO assembly.

The two `module2` lines are what make `build/pureunix.iso` a standalone bootable image: `fat.img`/`root.img` (copies of `build/pureunix.img`/`build/ext2.img`) are baked into the ISO and loaded into RAM by GRUB alongside the kernel, rather than being attached to QEMU as separate `-drive` files. The kernel discovers them via the Multiboot2 module tags below.

---

## Boot Modules (`fat.img` / `root.img`)

Multiboot2 tag type 3 (`MULTIBOOT2_TAG_MODULE`, `include/pureunix/multiboot.h`) carries a `(mod_start, mod_end, cmdline)` triple per `module2` line — `cmdline` here is just the string that followed the module's path in `grub.cfg` (`"fat.img"`/`"root.img"`), used as a name, not an actual command line.

`kernel/pmm.c`'s `parse_multiboot2()` records every module tag it sees (up to `MAX_BOOT_MODULES`, currently 4) into a small in-kernel table, and `pmm_init()` calls `reserve_region()` on each module's physical range — the same mechanism that protects the kernel image and heap from `pmm_alloc_frame()` — so a module's bytes are never handed out as a free physical frame once GRUB has loaded it. `kernel_main()` then looks modules up by name via `pmm_module_get()`/`pmm_module_count()` (declared in `include/pureunix/memory.h`) and wraps each one as a `disk_device_t` with `ramdisk_attach()` (`drivers/ramdisk.c`) — a RAM-backed block device whose `read`/`write` are plain `memcpy`s against the module's identity-mapped physical address (see `kernel/vmm.c`'s 128 MiB identity map — no separate mapping step is needed since every module lives well under that ceiling). `fat16_mount()`/`ext2_mount()` then operate on that `disk_device_t` exactly as they would a real ATA disk.

Because module memory is reserved but not copied anywhere else, this doubles as the filesystems' backing store for the life of the boot: writes go straight to the module's RAM, so `/`, `/fat`, and everything on them behave identically to `make run` before this change — including writes — for the duration of that boot session. Like any live CD/USB image, none of it is written back to `build/pureunix.iso` itself; a fresh boot starts from the image's original contents again.

One subtlety this creates: `kernel/heap.c`'s 8 MiB heap can no longer assume it starts right after `__kernel_end` — GRUB may have placed a module there. `heap_reserved_range()` instead starts the heap after whichever is higher of `__kernel_end` or `pmm_modules_end()` (the highest recorded module end address), computed once `pmm_init()` has already parsed the Multiboot2 module tags. See `docs/memory.md`'s Kernel Heap section.

If no module matching a known name is present — e.g. a bare `qemu-system-i386 -kernel build/pureunix.elf` invocation with no GRUB at all, which never produces module tags — `kernel_main()` falls back to `ata_primary_master()`/`ata_primary_slave()`, so the original ATA disk path (`drivers/ata.c`, unchanged) is still fully functional for real IDE hardware or hand-built QEMU setups that pass `-drive ...,if=ide`.

---

## GDT Initialization

`arch_init()` calls `gdt_init()` first. The GDT has 5 entries:

| Index | Selector | Base | Limit | Access | Description |
|---|---|---|---|---|---|
| 0 | `0x00` | 0 | 0 | `0x00` | Null descriptor |
| 1 | `0x08` | 0 | 4 GiB | `0x9A` | Kernel code (ring 0, execute/read) |
| 2 | `0x10` | 0 | 4 GiB | `0x92` | Kernel data (ring 0, read/write) |
| 3 | `0x18` | 0 | 4 GiB | `0xFA` | User code (ring 3, execute/read) |
| 4 | `0x20` | 0 | 4 GiB | `0xF2` | User data (ring 3, read/write) |

All segments use granularity `0xCF`: 4 KiB pages, 32-bit default operand size, limit upper nibble `0xF`.

`gdt_flush` (in `arch/i386/gdt_flush.S`) loads the GDT via `lgdt`, reloads `DS`, `ES`, `FS`, `GS`, `SS` with selector `0x10` (kernel data), then performs a far jump to selector `0x08` (kernel code) to reload `CS`.

---

## IDT Initialization

`idt_init()` populates all 256 IDT entries:

| Range | Type | Vector offsets | Handler |
|---|---|---|---|
| ISR 0–31 | Interrupt gate (flags `0x8E`, DPL=0) | 0–31 | CPU exceptions |
| IRQ 0–15 | Interrupt gate (flags `0x8E`, DPL=0) | 32–47 | Hardware interrupts |
| INT 0x80 | Interrupt gate (flags `0xEE`, DPL=3) | 128 | System calls |

The DPL=3 flag on entry 128 allows ring-3 code to issue `int $0x80` without a general protection fault.

After loading the IDTR via `idt_load` (which executes `lidt`), interrupts remain disabled until `arch_enable_interrupts()` at the end of `kernel_main`.

---

## PIC Remapping

The 8259A PIC is remapped so that hardware interrupts do not collide with CPU exception vectors (0–31). `pic_init()` sends the ICW sequence:

| Step | Port | Value | Meaning |
|---|---|---|---|
| ICW1 | PIC1=0x20, PIC2=0xA0 | `0x11` | Initialize, cascade, ICW4 expected |
| ICW2 | PIC1+1=0x21 | `0x20` | Master vector base = 32 |
| ICW2 | PIC2+1=0xA1 | `0x28` | Slave vector base = 40 |
| ICW3 | PIC1+1 | `0x04` | Slave connected on IRQ2 |
| ICW3 | PIC2+1 | `0x02` | Slave identity = 2 |
| ICW4 | both | `0x01` | 8086 mode |

The original IMR (interrupt mask register) values are saved before initialization and restored after, preserving any masks set before `pic_init` was called.

---

## PIT Initialization

`pit_init(100)` programs PIT channel 0 (port `0x40`) in mode 3 (square wave) at 100 Hz:

```
divisor = 1193182 / 100 = 11931
outb(0x43, 0x36)                  ; channel 0, mode 3, binary
outb(0x40, divisor & 0xFF)        ; low byte
outb(0x40, (divisor >> 8) & 0xFF) ; high byte
```

The IRQ0 handler increments a `uint64_t ticks` counter. `pit_sleep(ms)` busy-waits until the target tick is reached using `arch_halt()` in the wait loop.

---

## Enabling Interrupts

`arch_enable_interrupts()` is called at the very end of `kernel_main`, after all subsystems are initialized and all IRQ handlers are registered. It executes `sti`. From this point, the PIT, keyboard, and ATA IRQ handlers may fire.
