#!/usr/bin/env python3
import os
import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 65536
SPC = 4
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
MEDIA = 0xF8


def le16(x):
    return struct.pack("<H", x)


def le32(x):
    return struct.pack("<I", x)


class Fat16:
    def __init__(self):
        self.root_dir_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
        spf = 1
        while True:
            clusters = (TOTAL_SECTORS - RESERVED - FATS * spf - self.root_dir_sectors) // SPC
            needed = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if needed == spf:
                break
            spf = needed
        self.spf = spf
        self.first_fat = RESERVED
        self.root_sector = self.first_fat + FATS * self.spf
        self.first_data = self.root_sector + self.root_dir_sectors
        self.cluster_count = (TOTAL_SECTORS - self.first_data) // SPC
        self.next_cluster = 2
        self.image = bytearray(TOTAL_SECTORS * SECTOR)
        self.format()

    def sector_off(self, sector):
        return sector * SECTOR

    def cluster_sector(self, cluster):
        return self.first_data + (cluster - 2) * SPC

    def cluster_off(self, cluster):
        return self.sector_off(self.cluster_sector(cluster))

    def format(self):
        b = self.image
        b[0:3] = b"\xEB\x3C\x90"
        b[3:11] = b"PUREUNIX"
        b[11:13] = le16(SECTOR)
        b[13] = SPC
        b[14:16] = le16(RESERVED)
        b[16] = FATS
        b[17:19] = le16(ROOT_ENTRIES)
        b[19:21] = le16(TOTAL_SECTORS if TOTAL_SECTORS < 65536 else 0)
        b[21] = MEDIA
        b[22:24] = le16(self.spf)
        b[24:26] = le16(63)
        b[26:28] = le16(16)
        b[28:32] = le32(0)
        b[32:36] = le32(TOTAL_SECTORS)
        b[36] = 0x80
        b[38] = 0x29
        b[39:43] = le32(0x50555831)
        b[43:54] = b"PUREUNIX   "
        b[54:62] = b"FAT16   "
        b[510:512] = b"\x55\xAA"
        self.fat_set(0, 0xFFF8)
        self.fat_set(1, 0xFFFF)

    def fat_offset(self, fat_index, cluster):
        sector = self.first_fat + fat_index * self.spf
        return self.sector_off(sector) + cluster * 2

    def fat_set(self, cluster, value):
        for fat in range(FATS):
            off = self.fat_offset(fat, cluster)
            self.image[off:off + 2] = le16(value)

    def alloc_chain(self, data=b""):
        if not data:
            return 0
        clusters_needed = (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR)
        first = 0
        prev = 0
        pos = 0
        for _ in range(clusters_needed):
            c = self.next_cluster
            self.next_cluster += 1
            if not first:
                first = c
            if prev:
                self.fat_set(prev, c)
            self.fat_set(c, 0xFFFF)
            off = self.cluster_off(c)
            chunk = data[pos:pos + SPC * SECTOR]
            self.image[off:off + len(chunk)] = chunk
            pos += len(chunk)
            prev = c
        return first

    def name83(self, name):
        if name == ".":
            return "." + " " * 10
        if name == "..":
            return ".." + " " * 9
        name = name.upper()
        if "." in name:
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
        base = "".join(c for c in base if c not in " /\\")[:8]
        ext = "".join(c for c in ext if c not in " /\\")[:3]
        return base.ljust(8) + ext.ljust(3)

    def dir_entry(self, name, attr, cluster=0, size=0):
        e = bytearray(32)
        e[0:11] = self.name83(name).encode("ascii")
        e[11] = attr
        e[20:22] = le16((cluster >> 16) & 0xFFFF)
        e[26:28] = le16(cluster & 0xFFFF)
        e[28:32] = le32(size)
        return e

    def put_entry(self, dir_cluster, index, entry):
        if dir_cluster == 0:
            off = self.sector_off(self.root_sector) + index * 32
        else:
            off = self.cluster_off(dir_cluster) + index * 32
        self.image[off:off + 32] = entry

    def mkdir_root(self, name):
        cluster = self.alloc_chain(bytes(SPC * SECTOR))
        self.put_entry(0, self.next_root_index(), self.dir_entry(name, 0x10, cluster, 0))
        self.put_entry(cluster, 0, self.dir_entry(".", 0x10, cluster, 0))
        self.put_entry(cluster, 1, self.dir_entry("..", 0x10, 0, 0))
        return cluster

    def next_root_index(self):
        root = self.sector_off(self.root_sector)
        for i in range(ROOT_ENTRIES):
            if self.image[root + i * 32] == 0:
                return i
        raise RuntimeError("root directory full")

    def next_dir_index(self, cluster):
        off = self.cluster_off(cluster)
        for i in range((SPC * SECTOR) // 32):
            if self.image[off + i * 32] == 0:
                return i
        raise RuntimeError("directory full")

    def add_file(self, dir_cluster, name, data):
        cluster = self.alloc_chain(data)
        self.put_entry(dir_cluster, self.next_dir_index(dir_cluster), self.dir_entry(name, 0x20, cluster, len(data)))

    def add_root_file(self, name, data):
        cluster = self.alloc_chain(data)
        self.put_entry(0, self.next_root_index(), self.dir_entry(name, 0x20, cluster, len(data)))

    def write(self, path):
        with open(path, "wb") as f:
            f.write(self.image)


def main(argv):
    if len(argv) < 2:
        print("usage: mkfat16.py OUT.img [program.elf ...]", file=sys.stderr)
        return 2
    out = argv[1]
    image = Fat16()
    image.add_root_file(
        "README.TXT",
        b"PureUnix FAT16 root filesystem\n"
        b"Commands: help, ls, cat, touch, mkdir, rm, nano, hello, calc\n"
        b"FAT16 names are limited to 8.3 in this milestone.\n",
    )
    bin_cluster = image.mkdir_root("BIN")
    aliases = {
        "calc.elf": "CALC.ELF",
        "hello.elf": "HELLO.ELF",
        "viewer.elf": "VIEWER.ELF",
        "editor.elf": "EDITOR.ELF",
        "sh.elf": "SH.ELF",
    }
    for program in argv[2:]:
        base = os.path.basename(program).lower()
        name = aliases.get(base, base.upper())
        with open(program, "rb") as f:
            image.add_file(bin_cluster, name, f.read())
    image.write(out)
    print(f"created {out}: FAT16 {TOTAL_SECTORS * SECTOR // (1024 * 1024)} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
