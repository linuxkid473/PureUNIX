#!/usr/bin/env python3
"""
tools/mkdiskimg.py — assembles PureUNIX's persistent, USB-flashable disk
image: a real MBR (GRUB's boot.img bootstrap + our own one-entry partition
table), GRUB's core.img embedded raw in the ~1023 KiB MBR gap (LBA 1..2047),
and one writable EXT2 root partition (tools/mkext2.py's output, built with
--persistent-boot so it carries /boot/pureunix.elf and a real
/boot/grub/grub.cfg) starting at LBA 2048.

This is the "make iso produces a hybrid USB-bootable image" piece: dd this
file straight onto a USB stick and a real BIOS boots it exactly like any
other MBR disk — GRUB loads /boot/pureunix.elf from the EXT2 partition
itself (no multiboot2 ramdisk modules), and the kernel then mounts that
same partition as / directly (kernel_main()'s find_persistent_root_disk(),
kernel/main.c) — a real, writable, on-disk root that survives reboot.

Implements exactly what `grub-bios-setup` does for the i386-pc BIOS target
by hand (patch boot.img's embedded kernel-sector/boot-drive/drive-check
fields, patch core.img's embedded single-entry blocklist, place both at
their fixed disk locations) rather than shelling out to grub-bios-setup
itself — that tool insists on resolving its DEVICE argument against real,
mounted OS block devices (it walks /dev trying to "guess the root device")
and has no working path for targeting a bare image file on this project's
macOS/Homebrew cross-toolchain (confirmed by hand: it hard-errors with
"guessing the root device failed" even with --skip-fs-probe/--force against
a plain regular file and an explicit --device-map). The byte offsets and
blocklist format below are GRUB's own long-stable, documented i386-pc
install-time embedding protocol (include/grub/i386/pc/boot.h's
GRUB_BOOT_MACHINE_* constants and include/grub/offsets.h's
GRUB_BOOT_I386_PC_KERNEL_SEG, cross-referenced against the patch sequence
in util/setup.c) — not guessed.
"""
import argparse
import struct
import sys

SECTOR_SIZE = 512

# ---- MBR gap layout -------------------------------------------------------
CORE_IMG_START_LBA = 1        # right after the MBR sector
PART_START_LBA     = 2048     # 1 MiB aligned — matches user/install.c's
                               # original convention (kept for consistency
                               # with the never-wired-in installer design)

# ---- GRUB i386-pc boot.img patch offsets (include/grub/i386/pc/boot.h) ---
KERNEL_SECTOR_OFFSET = 0x5c   # 8 bytes LE: absolute LBA of core.img's 1st sector
BOOT_DRIVE_OFFSET    = 0x64   # 1 byte: 0xFF = "use whatever BIOS passed in DL"
DRIVE_CHECK_OFFSET   = 0x66   # 2 bytes: NOP'd out for a non-floppy (hard disk) install
PART_TABLE_OFFSET    = 446    # GRUB_BOOT_MACHINE_PART_START
MBR_SIGNATURE_OFFSET = 510

# ---- core.img embedded blocklist (grub-core/boot/i386/pc/diskboot.S) -----
BLOCKLIST_ENTRY_SIZE = 12     # packed {start: u64 LE, len: u16 LE, segment: u16 LE}
KERNEL_SEG            = 0x800 # GRUB_BOOT_I386_PC_KERNEL_SEG (include/grub/offsets.h)


def build_mbr(boot_img: bytes, part_sector_count: int) -> bytes:
    if len(boot_img) != SECTOR_SIZE:
        raise ValueError(f"boot.img must be exactly {SECTOR_SIZE} bytes, got {len(boot_img)}")

    mbr = bytearray(boot_img)

    struct.pack_into('<Q', mbr, KERNEL_SECTOR_OFFSET, CORE_IMG_START_LBA)
    mbr[BOOT_DRIVE_OFFSET] = 0xFF
    mbr[DRIVE_CHECK_OFFSET:DRIVE_CHECK_OFFSET + 2] = b'\x90\x90'

    # One real entry (bootable, type 0x83 Linux native); the other 3
    # primary slots stay zeroed.
    mbr[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 64] = b'\x00' * 64
    entry = struct.pack(
        '<BBBBBBBBII',
        0x80,                    # boot indicator: bootable
        0xFE, 0xFF, 0xFF,        # CHS start (placeholder; LBA fields below are authoritative)
        0x83,                    # partition type: Linux native
        0xFE, 0xFF, 0xFF,        # CHS end (placeholder)
        PART_START_LBA,
        part_sector_count,
    )
    mbr[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 16] = entry

    mbr[MBR_SIGNATURE_OFFSET] = 0x55
    mbr[MBR_SIGNATURE_OFFSET + 1] = 0xAA
    return bytes(mbr)


def patch_core_blocklist(core_img: bytes) -> bytes:
    """Embeds a single blocklist entry describing core.img's own on-disk
    sectors 2..N at the end of its first 512-byte sector (the "diskboot"
    loader), exactly where diskboot.S expects to find it. Sector 1 (the
    loader itself) is implicit — boot.img loads it directly via
    KERNEL_SECTOR_OFFSET, and it's the one that reads *this* blocklist to
    load the rest of core.img. core.img is always written as one
    contiguous run of sectors starting at CORE_IMG_START_LBA (see
    assemble()), so a single entry is always sufficient — no fragmentation
    is possible in this layout."""
    core = bytearray(core_img)
    total_sectors = (len(core) + SECTOR_SIZE - 1) // SECTOR_SIZE
    core.extend(b'\x00' * (total_sectors * SECTOR_SIZE - len(core)))

    entry_off = SECTOR_SIZE - BLOCKLIST_ENTRY_SIZE
    struct.pack_into(
        '<QHH', core, entry_off,
        CORE_IMG_START_LBA + 1,             # disk LBA of core.img's 2nd on-disk sector
        total_sectors - 1,                  # every sector after the first
        KERNEL_SEG + (SECTOR_SIZE >> 4),
    )
    # Terminator (len == 0) immediately before it — already zero from
    # grub-mkimage's own output, reasserted here defensively.
    term_off = entry_off - BLOCKLIST_ENTRY_SIZE
    struct.pack_into('<QHH', core, term_off, 0, 0, 0)
    return bytes(core)


def assemble(out_path: str, boot_img_path: str, core_img_path: str, ext2_img_path: str):
    with open(boot_img_path, 'rb') as f:
        boot_img = f.read()
    with open(core_img_path, 'rb') as f:
        core_img = f.read()
    with open(ext2_img_path, 'rb') as f:
        ext2_img = f.read()

    if len(ext2_img) % SECTOR_SIZE != 0:
        raise ValueError(f"{ext2_img_path} size {len(ext2_img)} is not sector-aligned")
    part_sector_count = len(ext2_img) // SECTOR_SIZE

    patched_core = patch_core_blocklist(core_img)
    gap_bytes = (PART_START_LBA - CORE_IMG_START_LBA) * SECTOR_SIZE
    if len(patched_core) > gap_bytes:
        raise ValueError(
            f"core.img ({len(patched_core)} bytes) doesn't fit the "
            f"{gap_bytes}-byte MBR gap before the partition at LBA {PART_START_LBA} "
            f"— grow PART_START_LBA")

    mbr = build_mbr(boot_img, part_sector_count)

    total_size = PART_START_LBA * SECTOR_SIZE + len(ext2_img)
    image = bytearray(total_size)
    image[0:SECTOR_SIZE] = mbr
    core_off = CORE_IMG_START_LBA * SECTOR_SIZE
    image[core_off:core_off + len(patched_core)] = patched_core
    part_off = PART_START_LBA * SECTOR_SIZE
    image[part_off:part_off + len(ext2_img)] = ext2_img

    with open(out_path, 'wb') as f:
        f.write(image)

    total_mb = total_size / (1024 * 1024)
    print(f"created {out_path}: {total_mb:.1f} MiB persistent disk image "
          f"(MBR+GRUB core.img in LBA 1-{PART_START_LBA - 1}, "
          f"EXT2 root partition at LBA {PART_START_LBA}, {part_sector_count} sectors)")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out')
    ap.add_argument('--boot-img', required=True)
    ap.add_argument('--core-img', required=True)
    ap.add_argument('--ext2-img', required=True)
    args = ap.parse_args(argv[1:])
    assemble(args.out, args.boot_img, args.core_img, args.ext2_img)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
