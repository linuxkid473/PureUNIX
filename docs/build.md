# Build Instructions

## Toolchain

Install or provide these tools on `PATH`:

- `i686-elf-gcc`
- `i686-elf-as`
- `make`
- `python3`
- `qemu-system-i386`

Optional:

- `grub-mkrescue` for `make iso`
- `xorriso`, usually used by GRUB tooling

## Targets

```sh
make
```

Builds:

- `build/pureunix.elf`
- user demo ELF programs under `build/user`
- `build/pureunix.img`, a FAT16 disk image

```sh
make run
```

Runs QEMU with:

- the kernel loaded directly
- 128 MiB RAM
- the generated FAT16 image attached as IDE primary master

```sh
make iso
```

Builds a GRUB ISO at `build/pureunix.iso`. This requires `grub-mkrescue`.

```sh
make clean
```

Removes build outputs.

## Notes

The kernel contains both Multiboot2 and Multiboot v1 headers. GRUB uses Multiboot2. QEMU direct loading is kept for fast development where GRUB tools are unavailable.

