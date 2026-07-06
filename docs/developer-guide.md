# Developer Guide

## Coding Style

- C99 with GCC extensions (`-std=gnu99`).
- 4-space indentation; no tabs in C files.
- Function names: `module_verb_noun` (e.g., `fat16_read_file`, `pit_sleep`, `ext2_read_block`).
- Struct types: `snake_case_t`.
- Enum constants: `ALL_CAPS`.
- `#define` constants: `ALL_CAPS`, prefixed with module name.
- Header guards: `__PUREUNIX_MODULE_H__`.
- Kernel-only declarations guarded with `#ifdef __PUREUNIX_KERNEL__`.
- No dynamic dispatch; function pointers only where unavoidable (VFS callbacks, `disk_device_t`).

---

## Adding a Device Driver

1. Create `drivers/<name>.c` and `include/pureunix/<name>.h`.
2. Write an init function (`<name>_init()`).
3. If the driver needs an IRQ:
   - Register with `idt_set_irq_handler(irq_num, handler_fn)`.
   - Enable the IRQ line with `irq_enable(irq_num)` from `arch/i386/pic.c`.
4. Call `<name>_init()` from `kernel/main.c` in the appropriate sequence (after `arch_init()` for hardware access, after `heap_init()` if the driver needs `kmalloc`).
5. Add the new `.c` file to one of the directories already included in the Makefile's `find` glob (`drivers/` is already covered).

No Makefile changes are needed; the build discovers all `.c` files under `kernel/`, `arch/`, `drivers/`, `fs/`, `libc/`, `shell/`, and `editor/` automatically.

---

## Adding a System Call

1. Add a new constant to `include/pureunix/syscall.h`:
   ```c
   SYS_<NAME> = <next_number>,
   ```
2. Add a case to `syscall_dispatch` in `arch/i386/syscall.c`. The function returns `uint32_t`; use `return` not `break`:
   ```c
   case SYS_<NAME>: {
       /* read args from regs->ebx, regs->ecx, regs->edx */
       return (uint32_t)<result>;
   }
   ```
3. If the syscall is called from user programs, add a wrapper to `user/libpure.c` and declare it in `user/libpure.h`. All wrappers go through the shared `syscall3` helper — no new inline assembly needed.
4. Mirror the same constant in `user/libpure.h` (the `SYS_*` defines are duplicated there because user programs cannot include kernel headers).

The INT 0x80 gate is already installed with DPL=3; no IDT changes are required for new syscall numbers.

---

## Adding a Shell Built-in

1. Write a function with the signature:
   ```c
   static int cmd_<name>(shell_context_t *ctx, shell_command_t *cmd,
                          const char *input, shell_output_t *out);
   ```
2. Add an entry to the `builtins[]` table in `shell/builtins.c`:
   ```c
   { "<name>", "<one-line description>", cmd_<name> },
   ```
3. All output must go through `shell_out_printf` or `shell_out_puts` — do not call `vga_write` directly. This is required for pipeline output to work correctly.
4. Access arguments via `cmd->argv[0..argc-1]` and `cmd->argc`.

---

## Adding a User Program

1. Create `user/<name>.c`. Include `user/libpure.h` for the runtime library.
2. Write a `main(void)` function returning `int` — or `main(int argc, char *argv[])` if the program needs its arguments; both work (see "argv" below).
3. Add the program name to `USER_PROGRAMS` in `Makefile`:
   ```makefile
   USER_PROGRAMS := hello calc viewer editor sh opentest readtest ext2test <name>
   ```
4. `make` will compile, link, and include it in the FAT16 image under `/BIN/<NAME>.ELF`.

**Multi-file programs**: if a program is more than one `.c` file (see `user/vi/`, Neatvi's vendored ~20-file source), the single-file `%.o`/`%.elf` pattern rules above don't apply — add its own `VI_SRCS`/`VI_OBJS`-style object list and an explicit `$(BUILD)/user/<name>.elf` link rule instead (an explicit rule takes precedence over the generic pattern rule for that one target). See the Makefile's "Neatvi" section for the full pattern.

**argv**: `user/crt0.S`'s `_start` re-pushes the `argc`/`argv` that `kernel/elf.c`'s `elf_load_into()` places on every new process's initial stack, so `main(int argc, char *argv[])` sees real values — `argv[0]` is the invoked name, `argv[1..]` whatever the shell (or another caller of `elf_exec_argv`) passed after it. `elf_exec(path)` (used by callers that don't have real args to pass) is just `elf_exec_argv(path, 1, {path, NULL})`.

**Constraints**:
- No standard library. Only `libpure` functions are available (unless the program opts into newlib — see `docs/userland.md`'s "A real C library (newlib)").
- Programs run in ring 3, each in its own address space (see `docs/userland.md`); there is no isolation *between* the code/data of concurrently-loaded programs beyond that.
- File I/O: `pu_open` (EXT2 or FAT16), `pu_read`, `pu_write`, `pu_lseek`, `pu_close`, `pu_stat` all work.
- No PATH search, no `pipe()`/`dup()`; `pu_exec()` replaces the caller with a single path and no argv of its own (see `user/vi/cmd.c` for what that rules out for a shelled-out `:!cmd`-style program).

---

## EXT2 Block Cache Rules

The EXT2 block cache returns non-owning pointers. These rules must be followed:

1. **Never call `kfree` on a pointer returned by `ext2_read_block`.** The cache owns the buffer.
2. **Copy data before calling `ext2_read_block` again.** Any `ext2_read_block` call can evict the slot holding your pointer. In `ext2_iter_blocks`, the singly-indirect block pointer table is copied into a `uint32_t local_ptrs[256]` array for exactly this reason.
3. **`ext2_block_cache_flush()`** is called at mount and unmount. Do not call it during normal driver operation.

---

## Initialization Order

The `kernel_main` function in `kernel/main.c` initializes subsystems in this order:

```
serial_init()             # COM1; needed for early debug output
vga_init()                # text mode; screen cleared
arch_init()               # GDT + IDT installed; interrupts still disabled
pmm_init()                # parses Multiboot memory map; bitmap populated
vmm_init()                # paging enabled
heap_init()               # kmalloc available after this point
tasking_init()            # initial "kernel" task created
syscall_init()            # no-op; INT 0x80 gate already set in idt_init
pit_init(100)             # IRQ0 handler registered; 100 Hz tick counter
keyboard_init()           # IRQ1 handler registered
ata_init()                # ATA primary master + slave probed; IRQ14 registered
vfs_init()                # VFS state initialized (no disk yet)
fat16_mount(primary_master)   # FAT16 filesystem; program store
ext2_mount(primary_slave)     # EXT2 filesystem; data store (may be absent)
arch_enable_interrupts()  # STI; hardware interrupts now active
shell_run()               # interactive shell loop; never returns
```

Subsystems may not be called out of order. In particular:
- `kmalloc` before `heap_init()` will dereference null.
- `irq_enable` before `arch_init()` will modify an uninitialized IDT.
- `fat16_mount` / `ext2_mount` before `ata_init()` will access an uninitialized disk device.
- `shell_run` before `arch_enable_interrupts()` will hang on the first keyboard read.

---

## Memory Regions

| Range | Owner |
|---|---|
| `0x00000000–0x000FFFFF` | BIOS/VGA/reserved; PMM never allocates here |
| `0x00100000–__kernel_end` | Kernel image; PMM marks as used |
| `__kernel_end – __kernel_end+8MB` | Kernel heap (fixed 8 MiB) |
| `0x00400000–0x006FFFFF` | ELF user program load range |
| `0x00B8000` | VGA framebuffer (within identity-mapped 128 MiB) |

The EXT2 block cache (16 KiB) lives in kernel `.bss` and does not use the heap.

---

## Debugging

### Serial Console

Run QEMU with `-serial stdio` (already in `make run`). All VGA output is mirrored to COM1 at 38400 baud. Serial output can be redirected:

```sh
make run 2>/dev/null | tee boot.log
```

### Kernel Panics

`panic(fmt, ...)` in `kernel/panic.c`:
- Disables interrupts.
- Writes white-on-red text to VGA.
- Writes the same text to serial.
- Halts indefinitely.

QEMU's `-no-reboot -no-shutdown` prevents the window from closing on a triple fault.

### GDB

QEMU supports remote GDB:

```sh
qemu-system-i386 \
    -m 128M \
    -kernel build/pureunix.elf \
    -drive file=build/pureunix.img,format=raw,if=ide,index=0 \
    -drive file=build/ext2.img,format=raw,if=ide,index=1 \
    -serial stdio \
    -no-reboot -no-shutdown \
    -s -S
```

`-s`: listens on `localhost:1234` for GDB.
`-S`: pauses CPU at startup until GDB connects.

Connect with:

```sh
gdb build/pureunix.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

The ELF has debug symbols if compiled without stripping (the Makefile does not strip).

---

## Known Pitfalls

### No User/Kernel Separation

User ELF programs run at ring 0 in the kernel address space. A bad pointer in a user program corrupts kernel data structures. There is no page fault handler for user errors.

### Stack Overflow in Tasks

Task stacks are 64 KiB (raised from an original 16 KiB — see below) with no guard page. Deep recursion or large stack frames silently overwrite adjacent heap blocks. The heap magic value (`0xC0FFEE42`) in adjacent `heap_block_t` headers may be used to detect corruption manually.

This actually happened: the original 16 KiB budget silently overran during the BusyBox port's `SYS_PIPE`/`SYS_DUP`/`SYS_DUP2` work, corrupting kernel memory as far away as EXT2's block group descriptor table, with symptoms (`[ext2] block: read of block 0 rejected`, spurious `-ENOENT`s) that pointed everywhere except the stack. The trigger: EXT2's own functions keep whole block-sized buffers on the stack rather than sharing one (`fs/ext2/alloc.c`'s `ext2_alloc_block()` alone stacks two 4 KiB locals), and the deepest real call chain — creating a file that triggers directory growth, `SYS_OPEN` → `vfs_create()` → `ext2_create()` → `ext2_dir_insert()` → `ext2_alloc_block()` — stacks three or more of these on top of each other, on top of the interrupt frame every syscall already carries (there is no separate per-syscall stack — see `kernel/task.c`'s `TASK_STACK_SIZE`). `user/systest.c`'s 200-file directory-growth stress section was enough to trip it. If stack corruption resurfaces after further changes here, suspect this exact chain first.

### Cooperative Scheduling Only

The PIT IRQ0 handler only increments `ticks`. A task that does not call `task_yield()` holds the CPU indefinitely.

### SYS_EXIT Does Not Kill the Task

Calling `syscall3(SYS_EXIT, code, 0, 0)` returns `code` from `syscall_dispatch` but does not terminate or zombie the current task. Programs relying on `exit()` semantics must avoid this.

### 8 MiB Heap, No Growth

The heap is fixed at 8 MiB starting at `__kernel_end`. Large `kmalloc` calls will fail (return null) if the heap is exhausted.

### EXT2 Block Cache Eviction

The 4-slot LRU block cache means only 4 distinct blocks can be live simultaneously. Code that holds a non-owning pointer from `ext2_read_block` and then calls `ext2_read_block` for a 5th distinct block will get the pointer silently invalidated. Always copy needed data into a local buffer or stack array before the next call.

### FAT16 8.3 Only

Filenames longer than 8 characters or extensions longer than 3 characters are truncated or rejected. LFN entries from other OS tools are skipped.

### Single-Level Undo in Editor

The editor keeps only one undo level. After a second edit, the previous undo state is permanently lost.

---

## Testing

Manual testing procedure:

1. `make` — verify zero errors and zero warnings.
2. `make run` — observe boot sequence on serial output and VGA.
3. At the shell prompt:
   - `help` — verify builtin list appears.
   - `ls /` — verify entries from both EXT2 (README.TXT, etc/, home/, testdir/, bigfile.bin, hugefile.bin) and FAT16 (BIN/, DOCS/) appear.
   - `cat /README.TXT` — verify EXT2 file read works.
   - `ls /bin` — verify FAT16 /BIN/ is readable.
   - `echo hello > /test.txt && cat /test.txt` — verify FAT16 write and pipeline.
   - `stat /bigfile.bin` — verify size 5120 from EXT2.
   - `stat /hugefile.bin` — verify size 14336 from EXT2.
   - `/bin/hello.elf` — verify ELF execution.
   - `/bin/opentest.elf` — verify file open/stat/lseek/close syscalls.
   - `/bin/readtest.elf` — verify SYS_READ on VFS-backed file descriptors.
   - `/bin/ext2test.elf` — 14-case EXT2 integration test; all should pass.
   - `ps` — verify task list.
   - `vim /test.txt` — verify editor opens and `:q` exits.
   - `reboot` — verify reboot via keyboard controller.
