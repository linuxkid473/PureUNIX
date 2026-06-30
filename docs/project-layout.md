# Project Layout

```
PureUNIX/
├── boot/
│   ├── multiboot2.S    Entry point (_start), Multiboot1+2 headers, 32KB stack
│   ├── linker.ld       Kernel linker script (load at 1MB, exports __kernel_start/__kernel_end)
│   └── grub.cfg        GRUB menu entry for ISO boot
│
├── arch/
│   └── i386/
│       ├── gdt.c               GDT setup (5 entries: null, kernel code/data, user code/data)
│       ├── idt.c               IDT setup, ISR/IRQ handler registration, isr_dispatch
│       ├── pic.c               8259A PIC initialization, EOI, IRQ enable/disable
│       ├── pit.c               PIT channel 0 at 100Hz, tick counter, pit_sleep busy-wait
│       ├── syscall.c           INT 0x80 dispatch (5 syscalls)
│       ├── context_switch.S    cooperative context switch (pushfl/pusha/save-ESP/load-ESP/popa/popfl/ret)
│       └── interrupt_stubs.S   ISR_NOERR/ISR_ERR/IRQ macros, isr_common_stub
│
├── kernel/
│   ├── main.c          Kernel entry point; calls all init functions; starts shell
│   ├── pmm.c           Bitmap physical memory manager; parses Multiboot1+2 mmap
│   ├── vmm.c           Identity-mapped paging; 32 static page tables × 4MB = 128MB
│   ├── heap.c          Linked-list kernel heap; 8MB fixed at __kernel_end
│   ├── task.c          Cooperative round-robin scheduler; task_t list; context switch glue
│   ├── elf.c           ELF32 loader; validates and loads PT_LOAD segments to 0x400000–0x700000
│   ├── panic.c         panic() — white-on-red VGA + serial + halt
│   └── reboot.c        Keyboard controller reset; ACPI/QEMU power off ports
│
├── drivers/
│   ├── vga.c           80×25 VGA text mode; ANSI SGR colors; hardware cursor; serial mirror
│   ├── keyboard.c      PS/2 IRQ1 driver; scan set 1; 128-entry ring buffer; shift/caps
│   ├── serial.c        COM1 38400 baud; mirrors VGA output; ANSI cursor sequences
│   └── ata.c           ATA primary master PIO; LBA28; IDENTIFY; sector read/write
│
├── fs/
│   ├── fat16.c         FAT16 read/write; 8.3 filenames; path_lookup; cluster alloc/free
│   └── vfs.c           VFS wrapper; path normalization (./..) ; last_error
│
├── libc/
│   ├── string.c        memcpy, memset, memcmp, strlen, strcpy, strcat, strcmp, strtok_r, etc.
│   ├── printf.c        vprintf/printf/sprintf; formats: %c %s %d %i %u %x %X %p %%
│   └── stdlib.c        kmalloc/kfree/kcalloc/krealloc; itoa; atoi
│
├── shell/
│   ├── sh.c            Main loop; pipeline execution; external program dispatch
│   ├── parser.c        Tokenizer; pipeline/redirection/pipe parsing
│   ├── line.c          Interactive line editor; history (64×256); tab completion
│   ├── builtins.c      27 built-in commands; environment variable table (32 vars)
│   └── shell_internal.h Internal types: shell_command_t, shell_pipeline_t, shell_output_t
│
├── editor/
│   └── editor.c        vim-like modal editor; NORMAL/INSERT/COMMAND/VSEARCH modes;
│                       512 lines × 256 chars; single-level undo; forward/backward search
│
├── user/
│   ├── crt0.S          User entry stub: _start: call main; ret
│   ├── libpure.c       syscall3 wrapper; pu_write/pu_read/pu_puts/pu_puti/pu_strlen/pu_atoi
│   ├── libpure.h       libpure declarations for user programs
│   ├── linker.ld       User linker script (base 0x400000)
│   ├── hello.c         Functional: prints a greeting via pu_puts
│   ├── calc.c          Demo: hardcoded arithmetic with pu_puti
│   ├── viewer.c        Stub: prints "not yet implemented"
│   ├── editor.c        Stub: prints "not yet implemented"
│   └── sh.c            Stub: prints "not yet implemented"
│
├── include/
│   └── pureunix/
│       ├── arch.h      arch_init, arch_enable/disable_interrupts, arch_halt, arch_io
│       ├── config.h    PUREUNIX_VERSION, PUREUNIX_MAX_PATH, PUREUNIX_MAX_NAME
│       ├── types.h     uint8_t–uint64_t, size_t, bool, NULL, true, false
│       ├── memory.h    pmm_*, vmm_*, kmalloc/kfree/kcalloc/krealloc
│       ├── task.h      task_t, task_state_t, tasking_init, task_create, task_yield, task_exit, task_kill
│       ├── syscall.h   SYS_* constants
│       ├── vfs.h       vfs_stat_t, vfs_dirent_t, VFS_O_*, all vfs_* functions
│       ├── fat16.h     fat16_fs_t, fat_dir_entry_t, FAT_ATTR_*, all fat16_* functions
│       ├── disk.h      disk_device_t, ata_init, ata_primary_master
│       ├── keyboard.h  KEY_* constants, keyboard_init, keyboard_getkey, keyboard_try_getkey
│       ├── shell.h     shell_run, shell_execute_line
│       ├── vga.h       vga_color enum, vga_init, vga_putc, vga_clear, cursor functions
│       ├── serial.h    serial_init, serial_putc, serial_clear, serial_move_cursor
│       ├── editor.h    editor_open
│       ├── elf.h       elf_exec, Elf32_Ehdr/Phdr typedefs
│       ├── io.h        inb/outb/inw/outw port I/O inline functions
│       ├── multiboot.h Multiboot1 and Multiboot2 struct definitions
│       ├── panic.h     panic() macro/function
│       ├── stdio.h     printf, vprintf, sprintf, putchar
│       ├── stdlib.h    kmalloc, kfree, kcalloc, krealloc, atoi, itoa
│       ├── string.h    memcpy, memset, strlen, strcmp, strcpy, strcat, strtok_r, etc.
│       └── ctype.h     isdigit, isalpha, isspace, toupper, tolower
│
├── tools/
│   └── mkfat16.py      Host-side FAT16 image builder; 32MB; creates /BIN/ with ELF programs
│
├── docs/               This documentation directory
│   ├── architecture.md System overview, boot flow, subsystem map
│   ├── boot.md         Multiboot headers, _start, linker script, GDT/IDT/PIC/PIT
│   ├── memory.md       PMM, VMM, heap
│   ├── interrupts.md   IDT, ISR stubs, dispatch, exception table, IRQ assignments
│   ├── scheduler.md    task_t, context switch, yield, exit, limitations
│   ├── filesystem.md   VFS API, FAT16 internals, disk image structure
│   ├── syscalls.md     INT 0x80 ABI, 5 syscalls, libpure wrappers
│   ├── drivers.md      VGA, Serial, PS/2 Keyboard, ATA PIO
│   ├── shell.md        Parser, pipeline execution, line editor, builtins, environment
│   ├── userland.md     ELF loader, crt0, libpure, user programs
│   ├── build.md        Makefile, compiler flags, targets, disk image generation
│   ├── developer-guide.md  Adding drivers/syscalls/builtins/programs, debugging, pitfalls
│   ├── project-layout.md   This file
│   └── api/            Per-subsystem API reference
│
├── Makefile
└── README.md
```

---

## Key Build Outputs

| File | Description |
|---|---|
| `build/pureunix.elf` | Kernel ELF (stripped of debug info by default on release; see build.md) |
| `build/pureunix.img` | 32 MiB raw FAT16 disk image |
| `build/pureunix.iso` | Bootable ISO (GRUB + Multiboot2, `make iso` only) |
| `build/user/*.elf` | Userland ELF programs |

---

## Dependency Graph (High Level)

```
boot/multiboot2.S
    └─ calls kernel_main()
            ├─ arch/i386/gdt.c
            ├─ arch/i386/idt.c
            │       └─ arch/i386/interrupt_stubs.S
            │       └─ arch/i386/pic.c
            ├─ kernel/pmm.c (needs multiboot.h)
            ├─ kernel/vmm.c (needs pmm)
            ├─ kernel/heap.c (needs vmm)
            ├─ kernel/task.c (needs heap)
            ├─ arch/i386/syscall.c (needs task, vga, keyboard)
            ├─ arch/i386/pit.c (needs idt, pic)
            ├─ drivers/keyboard.c (needs idt, pic)
            ├─ drivers/ata.c (needs idt, pic)
            ├─ fs/vfs.c
            │       └─ fs/fat16.c (needs ata, heap)
            └─ shell/sh.c (needs vfs, keyboard, vga, task, elf)
                    ├─ shell/parser.c
                    ├─ shell/line.c
                    ├─ shell/builtins.c
                    │       └─ editor/editor.c
                    └─ kernel/elf.c (needs vmm, heap)
```
