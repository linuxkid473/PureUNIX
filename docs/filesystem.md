# Filesystem Documentation

PureUnix mounts a FAT16 filesystem as `/`.

## Supported Operations

Through the VFS and FAT16 driver:

- mount root filesystem
- read directories
- create files
- delete files
- write files
- append files
- create subdirectories
- remove empty subdirectories
- rename/move entries
- query free/total space

## FAT16 Layout

The generated image uses:

- 512-byte sectors
- 4 sectors per cluster
- 2 FAT copies
- 512 root directory entries
- 32 MiB image size

The image builder is `tools/mkfat16.py`.

## Filename Limitations

This milestone intentionally supports classic FAT 8.3 filenames only.

Examples that work:

- `README.TXT`
- `HELLO.ELF`
- `NOTES.TXT`
- `DOCS`

Examples that do not work yet:

- `long-file-name.txt`
- names with spaces
- Unicode names

Long filename entries have attribute `0x0F`; the driver skips them when listing directories.

## Directory Rules

The root directory has a fixed capacity, as in FAT16. Subdirectories can grow by allocating new clusters.

`rmdir` refuses to remove non-empty directories.

