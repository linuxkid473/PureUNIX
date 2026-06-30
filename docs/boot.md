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
set timeout=0
set default=0

menuentry "PureUnix" {
    multiboot2 /boot/pureunix.elf
    boot
}
```

This is used only for ISO builds (`make iso`). The `make run` target uses QEMU's `-kernel` flag directly and bypasses GRUB.

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
