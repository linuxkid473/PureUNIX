# API Reference

Per-subsystem API reference for PureUNIX kernel internals.

| File | Contents |
|---|---|
| [memory.md](memory.md) | PMM (`pmm_*`), VMM (`vmm_*`), kernel heap (`kmalloc`, `kfree`, `kcalloc`, `krealloc`) |
| [task.md](task.md) | Task creation, scheduling, yield, exit, enumeration |
| [vfs.md](vfs.md) | VFS dual-dispatch (EXT2 + FAT16): file and directory operations, path normalization |
| [drivers.md](drivers.md) | VGA, Serial, PS/2 Keyboard, ATA PIO (master + slave), port I/O, interrupt registration, PIT |
| [libc.md](libc.md) | stdio (printf family), stdlib (atoi, strtol), string.h, ctype.h, panic |

These APIs are for kernel code only. User programs use `libpure` — see [../userland.md](../userland.md).
