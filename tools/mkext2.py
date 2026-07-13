#!/usr/bin/env python3
"""
mkext2.py — minimal EXT2 filesystem image builder for PureUNIX.

Creates a read-only EXT2 image suitable for testing the Stage 1 EXT2
driver.  Only the structures needed for reading are populated; journal,
extended attributes, and resize metadata are not used.

Layout (1 KB blocks, 3 block groups -- see "Why 3 block groups" below):
  Block 0  : boot block / padding (first 1024 bytes, before the superblock)
  Block 1  : superblock
  Block 2  : block group descriptor table (BGDT) -- one 32-byte entry per
             group, all 3 in this single block (3*32 = 96 bytes)
  Block 3  : group 0's block bitmap
  Block 4  : group 0's inode bitmap
  Block 5  : group 0's inode table (128 blocks for 1024 inodes x 128
             bytes/inode)
   ...
  Block 133: first free (data) block in group 0
   ...
  Block 8193 (group 1's first block): group 1's block bitmap
  Block 8194: group 1's inode bitmap
  Block 8195: group 1's inode table (128 blocks)
   ...
  Block 8323: first free (data) block in group 1
   ... (group 2 the same shape, starting at block 16385)

Why 3 block groups: a single block group's block bitmap is exactly one
block, so BLOCKS_PER_GROUP is capped at block_size * 8 -- a real EXT2
format constraint (also true of Linux's own ext2), not a shortcut this
generator takes. At 1 KB blocks that's an 8 MiB ceiling per group. Group 0's
on-disk layout/offsets are kept byte-for-byte identical to every previous
version of this file (nothing here was renumbered) specifically so the
regression suite's existing fixed-offset fixtures (bigfile.bin/hugefile.bin
block-boundary reads in user/ext2test.c and user/systest.c) keep passing
unmodified; groups 1 and 2 are purely additive extra capacity, added
because a real statically-linked SDL2 app (docs/sdl-port.md's
sdltest.elf) no longer fit in the old single-group 8 MiB image alongside
everything already installed. fs/ext2/alloc.c's block/inode allocator was
already fully group-generic (iterates fs->num_groups, indexes fs->bgdt[g])
before this file ever needed more than one group -- multi-group support
existed and was reachable via the write path (e.g. SQLite/ash creating new
files at runtime) even though no image had ever exercised more than group 0
until now.
"""
import struct, sys, os, time

# Real build-time timestamp (seconds since epoch), used for every inode's
# atime/mtime/ctime — a static image has no other authentic value to give
# these fields, but this is the actual time the image was built, not a
# fabricated placeholder like the all-zero timestamps used previously.
BUILD_TIME = int(time.time())

# ---- filesystem parameters -----------------------------------------------
BLOCK_SIZE        = 1024
SECTOR_SIZE       = 512
SECTORS_PER_BLOCK = BLOCK_SIZE // SECTOR_SIZE   # 2
EXT2_MAGIC        = 0xEF53
INODE_SIZE        = 128
INODES_PER_GROUP  = 1024
BLOCKS_PER_GROUP  = 8192          # max for 1 KB blocks (1 bitmap block = 8192 bits)
NUM_GROUPS        = 3             # see the module docstring's "Why 3 block
                                   # groups" -- 24 MB total, comfortably
                                   # fitting the SDL2 port's sdltest.elf
                                   # alongside everything else installed.
FIRST_DATA_BLOCK  = 1             # for 1 KB blocks the SB lives in block 1

# Block layout for group 0 -- unchanged from every previous single-group
# version of this file (see the module docstring).
BGDT_BLOCK        = 2
BLOCK_BITMAP_BLOCK= 3
INODE_BITMAP_BLOCK= 4
INODE_TABLE_BLOCK = 5
INODE_TABLE_BLOCKS= (INODES_PER_GROUP * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE  # 128
FIRST_FREE_BLOCK  = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS  # 133

# Reserved (metadata) blocks at the start of any group *other* than group 0:
# just its own block bitmap + inode bitmap + inode table (no per-group
# superblock/BGDT backup copies -- see the module docstring: this
# generator and PureUNIX's own fs/ext2/ reader are the only two things that
# ever look at this image, and the reader only ever consults the *primary*
# superblock/BGDT, so the redundant backup copies real e2fsck-compatible
# ext2 would require in every group are simply not needed here).
EXTRA_GROUP_RESERVED_BLOCKS = 2 + INODE_TABLE_BLOCKS  # 130

def group_first_block(g):
    """Absolute block number of group g's first block (its own bitmap, for
    every group but 0 -- group 0's first block is the boot block)."""
    return FIRST_DATA_BLOCK + g * BLOCKS_PER_GROUP

def group_meta_blocks(g):
    """(block_bitmap, inode_bitmap, inode_table_start) absolute block
    numbers for group g."""
    if g == 0:
        return BLOCK_BITMAP_BLOCK, INODE_BITMAP_BLOCK, INODE_TABLE_BLOCK
    base = group_first_block(g)
    return base, base + 1, base + 2

def group_first_free_block(g):
    """First block in group g available for file data (i.e. not this
    group's own bitmaps/inode table)."""
    if g == 0:
        return FIRST_FREE_BLOCK
    return group_first_block(g) + EXTRA_GROUP_RESERVED_BLOCKS

# NOTE: deliberately NUM_GROUPS * BLOCKS_PER_GROUP, *not*
# group_first_block(NUM_GROUPS) (= FIRST_DATA_BLOCK + NUM_GROUPS *
# BLOCKS_PER_GROUP). fs/ext2/super.c's own num_groups computation is
# `ceil(s_blocks_count / s_blocks_per_group)` — it does not subtract
# s_first_data_block first the way the real EXT2 spec's formula does. With
# FIRST_DATA_BLOCK=1 added in, s_blocks_count would be one more than an
# exact multiple of BLOCKS_PER_GROUP, and the kernel's ceiling division
# would then compute NUM_GROUPS+1 groups — a phantom extra group whose
# BGDT entry this generator never wrote (all-zero), which
# fs/ext2/alloc.c's allocator would then try to use as if it were real,
# corrupting allocation immediately (this was caught by SDL_GetWindowSurface
# failing with "Out of memory" during the SDL2 port's first QEMU boot test
# — docs/sdl-port.md). Using an exact multiple here instead means group
# NUM_GROUPS-1 ends up exactly one block short of a "full" BLOCKS_PER_GROUP
# (its last theoretical block, at group_first_block(NUM_GROUPS)-1
# overall, is simply never allocated) — harmless, and still matches the
# kernel's own group-count formula exactly.
TOTAL_BLOCKS = NUM_GROUPS * BLOCKS_PER_GROUP

# EXT2 inode mode bits
S_IFDIR = 0x4000
S_IFREG = 0x8000
S_IFLNK = 0xA000
S_IFCHR = 0x2000
S_IRWXU = 0o755    # owner rwx
DEFAULT_DIR_MODE  = S_IFDIR | 0o755
DEFAULT_FILE_MODE = S_IFREG | 0o644
DEFAULT_LINK_MODE = S_IFLNK | 0o777   # symlink permissions are conventionally all-access

# Directory entry file_type values
FT_UNKNOWN = 0
FT_REG     = 1
FT_DIR     = 2
FT_SYMLINK = 7

# Reserved inode numbers
ROOT_INO = 2


def le16(v): return struct.pack('<H', v & 0xFFFF)
def le32(v): return struct.pack('<I', v & 0xFFFFFFFF)


class Ext2Builder:
    def __init__(self, volume_label: str = 'PureUnixEXT2'):
        # Only the persistent-disk build (tools/mkdiskimg.py, via
        # --persistent-boot) overrides this to 'PUREUNIX_ROOT' — it's the
        # exact label boot/grub-embedded.cfg's `search --label` looks for
        # to find this partition after the MBR/core.img handoff. The
        # ordinary ISO+ramdisk build's label is unused by anything today,
        # left at its original value to keep that image byte-for-byte
        # unchanged.
        self.volume_label = volume_label
        self.image        = bytearray(TOTAL_BLOCKS * BLOCK_SIZE)
        self.next_block   = FIRST_FREE_BLOCK
        self.next_inode   = ROOT_INO + 1   # inode 2 is root; 3+ are free
        # Track usage for bitmaps — group 0's reserved range, plus groups
        # 1..NUM_GROUPS-1's own (bitmap + inode bitmap + inode table) range.
        self.used_blocks  = set(range(FIRST_FREE_BLOCK))
        for g in range(1, NUM_GROUPS):
            base = group_first_block(g)
            self.used_blocks |= set(range(base, base + EXTRA_GROUP_RESERVED_BLOCKS))
        self.used_inodes  = {1, ROOT_INO}  # 1=bad-blocks, 2=root

        # Directory entries per inode: inode_no -> list of (name, ino, ft)
        self._dirs = {}
        # Store (mode, size, blocks_list, nlinks, uid, gid) per inode
        self._inodes = {}
        # inode_no -> parent inode_no, used to fill in a directory's own ".."
        # entry with the real parent instead of always pointing at root.
        self._parent = {ROOT_INO: ROOT_INO}

        # Reserve root (inode 2) as a directory with no entries yet
        self._dirs[ROOT_INO] = []
        self._inodes[ROOT_INO] = {
            'mode': DEFAULT_DIR_MODE, 'size': 0, 'blocks': [],
            'nlinks': 2, 'uid': 0, 'gid': 0
        }

    # ------------------------------------------------------------------
    # Allocation
    # ------------------------------------------------------------------
    def _alloc_block(self):
        b = self.next_block
        # Jump over any group's own reserved (bitmap/inode-table) range —
        # next_block increments linearly across group boundaries, so once
        # it lands inside group g's metadata (blocks
        # [group_first_block(g), group_first_free_block(g))) it must skip
        # straight to that group's first free block instead of handing out
        # a metadata block as if it were file data.
        for g in range(1, NUM_GROUPS):
            if group_first_block(g) <= b < group_first_free_block(g):
                b = group_first_free_block(g)
                break
        if b >= TOTAL_BLOCKS:
            raise RuntimeError("EXT2 image full")
        self.next_block = b + 1
        self.used_blocks.add(b)
        return b

    def _alloc_inode(self):
        i = self.next_inode
        if i > NUM_GROUPS * INODES_PER_GROUP:
            raise RuntimeError("inode table full")
        self.next_inode += 1
        self.used_inodes.add(i)
        return i

    # ------------------------------------------------------------------
    # Raw block write
    # ------------------------------------------------------------------
    def _write_block(self, blk_no, data: bytes):
        start = blk_no * BLOCK_SIZE
        data  = bytes(data)[:BLOCK_SIZE]
        self.image[start : start + len(data)] = data

    # ------------------------------------------------------------------
    # High-level filesystem operations
    # ------------------------------------------------------------------
    def mkdir(self, parent_ino, name: str, mode=None) -> int:
        """Create a directory inside parent_ino; return the new inode number.

        mode defaults to DEFAULT_DIR_MODE (0755); pass an explicit
        S_IFDIR|0oNNN for a differently-permissioned directory (e.g. one
        with no execute bit, to exercise ancestor X_OK traversal denial)."""
        ino = self._alloc_inode()
        self._dirs[ino] = []
        self._inodes[ino] = {
            'mode': mode if mode is not None else DEFAULT_DIR_MODE, 'size': 0, 'blocks': [],
            'nlinks': 2, 'uid': 0, 'gid': 0
        }
        self._parent[ino] = parent_ino
        # Add entry in parent
        self._dirs[parent_ino].append((name, ino, FT_DIR))
        self._inodes[parent_ino]['nlinks'] += 1   # parent gets an extra link for '..'
        return ino

    def add_symlink(self, parent_ino, name: str, target: str) -> int:
        """Create a symlink inside parent_ino; return the new inode number.

        Uses ext2's "fast symlink" convention: for targets short enough to
        fit in the 60 bytes of i_block (no indirection needed), the target
        path is stored inline in the inode itself and no data block is
        allocated at all."""
        target_bytes = target.encode('ascii')
        assert len(target_bytes) <= 60, "symlink target too long for inline fast-symlink storage"
        ino = self._alloc_inode()
        self._inodes[ino] = {
            'mode': DEFAULT_LINK_MODE, 'size': len(target_bytes), 'blocks': [],
            'nlinks': 1, 'uid': 0, 'gid': 0, 'inline_target': target_bytes,
        }
        self._dirs[parent_ino].append((name, ino, FT_SYMLINK))
        return ino

    def add_file(self, parent_ino, name: str, data: bytes,
                 mode=None, uid: int = 0, gid: int = 0) -> int:
        """Create a regular file inside parent_ino; return the new inode number.

        mode defaults to DEFAULT_FILE_MODE (0644); pass an explicit
        S_IFREG|0oNNN to give a file different permissions (and optionally a
        non-root uid/gid), e.g. for permission-engine regression fixtures."""
        ino   = self._alloc_inode()
        # Allocate data blocks
        blk_list = []
        remaining = len(data)
        pos = 0
        while remaining > 0:
            blk = self._alloc_block()
            blk_list.append(blk)
            chunk = data[pos : pos + BLOCK_SIZE]
            self._write_block(blk, chunk.ljust(BLOCK_SIZE, b'\x00'))
            pos       += len(chunk)
            remaining -= len(chunk)
        self._inodes[ino] = {
            'mode': mode if mode is not None else DEFAULT_FILE_MODE,
            'size': len(data), 'blocks': blk_list,
            'nlinks': 1, 'uid': uid, 'gid': gid
        }
        self._dirs[parent_ino].append((name, ino, FT_REG))
        return ino

    # ------------------------------------------------------------------
    # Serialise a directory's entries into one or more data blocks
    # ------------------------------------------------------------------
    def _build_dir_blocks(self, entries_with_dots: list) -> list:
        """
        Pack directory entries across as many BLOCK_SIZE blocks as needed
        (real EXT2 directories grow this way once entries stop fitting in
        one block — same reason kernel/fs/ext2/dir.c's ext2_dir_insert()
        grows a directory at runtime, see docs/developer-guide.md's "Stack
        Overflow in Tasks" story for how that path was found/fixed). The
        last entry actually placed in each block has its rec_len stretched
        to fill that block, exactly matching a real EXT2 directory's
        on-disk layout — not just the final block.
        """
        remaining = list(entries_with_dots)
        blocks = []
        while remaining:
            buf = bytearray(BLOCK_SIZE)
            off = 0
            packed_total = 0
            while remaining:
                name, ino, ft = remaining[0]
                name_bytes = name.encode('ascii')
                rec_len = (8 + len(name_bytes) + 3) & ~3
                if off + rec_len > BLOCK_SIZE:
                    break
                struct.pack_into('<IHBBs', buf, off,
                                 ino, rec_len, len(name_bytes), ft, b'\x00')
                buf[off + 8 : off + 8 + len(name_bytes)] = name_bytes
                off += rec_len
                packed_total += 1
                remaining.pop(0)
            if packed_total == 0:
                raise ValueError(f"directory entry name too long to fit in a {BLOCK_SIZE}-byte block")
            # Stretch the last entry actually placed in this block to fill it.
            last_off = off
            # Walk back to find the last entry's header offset.
            prev_off = 0
            it = 0
            walk = 0
            while walk < off:
                rec_len = struct.unpack_from('<H', buf, walk + 4)[0]
                prev_off = walk
                walk += rec_len
                it += 1
            struct.pack_into('<H', buf, prev_off + 4, BLOCK_SIZE - prev_off)
            blocks.append(bytes(buf))
        return blocks

    def _finalise_dirs(self):
        """Allocate data blocks for all directories and write their entries."""
        for ino, entries in self._dirs.items():
            # Build the canonical entry list: "." and ".." come first
            dot_entries = [
                ('.',  ino, FT_DIR),
                ('..', self._parent.get(ino, ROOT_INO), FT_DIR),
            ] + entries
            block_datas = self._build_dir_blocks(dot_entries)
            blks = []
            for block_data in block_datas:
                blk = self._alloc_block()
                self._write_block(blk, block_data)
                blks.append(blk)
            self._inodes[ino]['blocks'] = blks
            self._inodes[ino]['size']   = BLOCK_SIZE * len(blks)

    # ------------------------------------------------------------------
    # Inode table serialisation
    # ------------------------------------------------------------------
    def _pack_inode(self, info: dict) -> bytes:
        mode    = info['mode']
        size    = info['size']
        uid     = info['uid']
        gid     = info['gid']
        nlinks  = info['nlinks']
        blks    = info['blocks']
        inline_target = info.get('inline_target')

        i_block        = [0] * 15
        i_blocks_field  = 0

        if inline_target is not None:
            # Fast symlink: the target path lives directly in the 60 bytes
            # of i_block: no data block is allocated, and i_blocks stays 0.
            padded = inline_target.ljust(60, b'\x00')
            i_block = list(struct.unpack('<15I', padded))
        else:
            # Build i_block[15]: direct[0..11], singly-indirect[12],
            # doubly-indirect[13]. PTRS_PER_BLOCK block pointers (4 bytes
            # each) fit in one BLOCK_SIZE block; direct + singly-indirect
            # addresses 12 + PTRS_PER_BLOCK blocks (268 for 1 KB blocks —
            # see docs/developer-guide.md). Anything beyond that needs
            # doubly-indirect: i_block[13] points to a block of pointers to
            # further singly-indirect blocks, mirroring the kernel's own
            # read-side support in fs/ext2/inode.c's ext2_iter_blocks().
            PTRS_PER_BLOCK = BLOCK_SIZE // 4

            direct    = blks[:12]
            rest      = blks[12:]
            singly    = rest[:PTRS_PER_BLOCK]
            doubly    = rest[PTRS_PER_BLOCK:]

            for idx, b in enumerate(direct):
                i_block[idx] = b

            meta_blocks = 0

            if singly:
                ind_blk_no = self._alloc_block()
                ind_buf    = bytearray(BLOCK_SIZE)
                for idx, b in enumerate(singly):
                    struct.pack_into('<I', ind_buf, idx * 4, b)
                self._write_block(ind_blk_no, bytes(ind_buf))
                i_block[12] = ind_blk_no
                meta_blocks += 1

            if doubly:
                if len(doubly) > PTRS_PER_BLOCK * PTRS_PER_BLOCK:
                    raise ValueError(
                        f"file needs {len(blks)} blocks, exceeding this "
                        f"builder's doubly-indirect capacity "
                        f"({12 + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK} blocks)")
                dind_buf = bytearray(BLOCK_SIZE)
                for chunk_idx in range(0, len(doubly), PTRS_PER_BLOCK):
                    chunk = doubly[chunk_idx : chunk_idx + PTRS_PER_BLOCK]
                    ind2_blk_no = self._alloc_block()
                    ind2_buf = bytearray(BLOCK_SIZE)
                    for idx, b in enumerate(chunk):
                        struct.pack_into('<I', ind2_buf, idx * 4, b)
                    self._write_block(ind2_blk_no, bytes(ind2_buf))
                    struct.pack_into('<I', dind_buf, (chunk_idx // PTRS_PER_BLOCK) * 4, ind2_blk_no)
                    meta_blocks += 1
                dind_blk_no = self._alloc_block()
                self._write_block(dind_blk_no, bytes(dind_buf))
                i_block[13] = dind_blk_no
                meta_blocks += 1

            # i_blocks: 512-byte units of all allocated blocks (data blocks
            # plus every indirect/doubly-indirect metadata block).
            total_disk_blocks = len(blks) + meta_blocks
            i_blocks_field    = total_disk_blocks * SECTORS_PER_BLOCK

        fmt = '<HHIIIIIHHIII' + 'I' * 15 + 'IIII12s'
        raw = struct.pack(fmt,
            mode, uid, size,
            BUILD_TIME,  # i_atime
            BUILD_TIME,  # i_ctime
            BUILD_TIME,  # i_mtime
            0,    # i_dtime
            gid, nlinks,
            i_blocks_field,
            0,    # i_flags
            0,    # i_osd1
            *i_block,
            0,    # i_generation
            0,    # i_file_acl
            0,    # i_dir_acl
            0,    # i_faddr
            b'\x00' * 12,  # i_osd2
        )
        assert len(raw) == INODE_SIZE, f"inode packing error: {len(raw)} != {INODE_SIZE}"
        return raw

    def _write_inode_table(self):
        for ino, info in self._inodes.items():
            g            = (ino - 1) // INODES_PER_GROUP
            local_idx    = (ino - 1) % INODES_PER_GROUP
            byte_off     = local_idx * INODE_SIZE
            blk_off      = byte_off // BLOCK_SIZE
            off_in_blk   = byte_off % BLOCK_SIZE
            _, _, inode_table_block = group_meta_blocks(g)
            abs_blk      = inode_table_block + blk_off
            raw          = self._pack_inode(info)
            start        = abs_blk * BLOCK_SIZE + off_in_blk
            self.image[start : start + INODE_SIZE] = raw

    # ------------------------------------------------------------------
    # Bitmap generation
    # ------------------------------------------------------------------
    def _write_bitmap(self, blk_no, used_set, max_bits):
        buf = bytearray(BLOCK_SIZE)
        for bit in used_set:
            if bit < max_bits:
                buf[bit // 8] |= (1 << (bit % 8))
        self._write_block(blk_no, bytes(buf))

    # ------------------------------------------------------------------
    # Superblock + BGDT
    # ------------------------------------------------------------------
    def _write_superblock(self):
        total_inodes     = NUM_GROUPS * INODES_PER_GROUP
        num_used_inodes  = len(self.used_inodes)
        num_used_blocks  = len(self.used_blocks)
        free_blocks      = TOTAL_BLOCKS - num_used_blocks
        free_inodes      = total_inodes - num_used_inodes

        # Superblock is 1024 bytes; pad with zeros to fill
        sb = bytearray(BLOCK_SIZE)

        def put32(off, v): struct.pack_into('<I', sb, off, v & 0xFFFFFFFF)
        def put16(off, v): struct.pack_into('<H', sb, off, v & 0xFFFF)
        def put8 (off, v): sb[off] = v & 0xFF

        put32(0,   total_inodes)              # s_inodes_count
        put32(4,   TOTAL_BLOCKS)              # s_blocks_count
        put32(8,   0)                         # s_r_blocks_count
        put32(12,  free_blocks)               # s_free_blocks_count
        put32(16,  free_inodes)               # s_free_inodes_count
        put32(20,  FIRST_DATA_BLOCK)          # s_first_data_block
        put32(24,  0)                         # s_log_block_size (0 → 1024)
        put32(28,  0)                         # s_log_frag_size
        put32(32,  BLOCKS_PER_GROUP)          # s_blocks_per_group
        put32(36,  BLOCKS_PER_GROUP)          # s_frags_per_group
        put32(40,  INODES_PER_GROUP)          # s_inodes_per_group
        put32(44,  0)                         # s_mtime
        put32(48,  0)                         # s_wtime
        put16(52,  0)                         # s_mnt_count
        put16(54,  0xFFFF)                    # s_max_mnt_count (no limit)
        put16(56,  EXT2_MAGIC)               # s_magic
        put16(58,  1)                         # s_state (clean)
        put16(60,  1)                         # s_errors (continue)
        put16(62,  0)                         # s_minor_rev_level
        put32(64,  0)                         # s_lastcheck
        put32(68,  0)                         # s_checkinterval
        put32(72,  0)                         # s_creator_os (Linux=0)
        put32(76,  1)                         # s_rev_level = DYNAMIC_REV
        put16(80,  0)                         # s_def_resuid
        put16(82,  0)                         # s_def_resgid
        # EXT2_DYNAMIC_REV fields (offset 84+)
        put32(84,  11)                        # s_first_ino
        put16(88,  INODE_SIZE)               # s_inode_size
        put16(90,  0)                         # s_block_group_nr
        put32(92,  0)                         # s_feature_compat
        put32(96,  0x0002)                    # s_feature_incompat = FILETYPE
        put32(100, 0)                         # s_feature_ro_compat
        # UUID: fixed non-zero value so the FS can be identified
        sb[104:120] = b'\x50\x55\x52\x45\x55\x4e\x49\x58\x45\x58\x54\x32\x00\x00\x00\x01'
        # Volume name (16 bytes, null-padded/truncated)
        label_bytes = self.volume_label.encode('ascii')[:16]
        sb[120:120 + len(label_bytes)] = label_bytes

        # Write to disk — the superblock occupies the SECOND 1 KB block
        # (byte offset 1024 = block 1 × 1024)
        start = FIRST_DATA_BLOCK * BLOCK_SIZE
        self.image[start : start + BLOCK_SIZE] = sb

    def _write_bgdt(self):
        # One 32-byte descriptor per group (real ext2_group_desc layout:
        # 3 x uint32 + 4 x uint16 + 3 x uint32 reserved = 32 bytes exactly —
        # see fs/ext2/ext2.h's ext2_bgdt_entry_t, which fs/ext2/alloc.c
        # indexes as fs->bgdt[g], so every entry must land at exactly
        # g*32, not the 22-byte stride a plain '<IIIHHHI' pack produces).
        entries = bytearray()
        for g in range(NUM_GROUPS):
            base = group_first_block(g)
            bbitmap_blk, ibitmap_blk, itable_blk = group_meta_blocks(g)
            used_blocks_in_group = sum(
                1 for blk in self.used_blocks if base <= blk < base + BLOCKS_PER_GROUP)
            free_blocks_in_group = BLOCKS_PER_GROUP - used_blocks_in_group
            ino_base = g * INODES_PER_GROUP
            used_inodes_in_group = sum(
                1 for i in self.used_inodes if ino_base < i <= ino_base + INODES_PER_GROUP)
            free_inodes_in_group = INODES_PER_GROUP - used_inodes_in_group
            used_dirs_in_group = sum(
                1 for ino in self._dirs if ino_base < ino <= ino_base + INODES_PER_GROUP)
            entries += struct.pack('<IIIHHHHIII',
                bbitmap_blk,             # bg_block_bitmap
                ibitmap_blk,             # bg_inode_bitmap
                itable_blk,              # bg_inode_table
                free_blocks_in_group,    # bg_free_blocks_count
                free_inodes_in_group,    # bg_free_inodes_count
                used_dirs_in_group,      # bg_used_dirs_count
                0,                       # bg_pad
                0, 0, 0,                 # bg_reserved[3]
            )
        buf = bytes(entries) + b'\x00' * (BLOCK_SIZE - len(entries))
        self._write_block(BGDT_BLOCK, buf)

    # ------------------------------------------------------------------
    # Finalise and emit
    # ------------------------------------------------------------------
    def build(self, out_path: str):
        self._finalise_dirs()
        self._write_inode_table()

        # Group 0: block bitmap bit N is set directly for absolute block N
        # (not group-0-relative, i.e. NOT subtracting FIRST_DATA_BLOCK) —
        # kept byte-for-byte identical to every previous single-group
        # version of this file (see the module docstring): blocks
        # 0..FIRST_FREE_BLOCK-1 are used, plus whatever data blocks were
        # allocated during finalise/file creation that landed in group 0's
        # own range. max_bits=BLOCKS_PER_GROUP (not the old TOTAL_BLOCKS,
        # now larger than one group) both keeps this identical to before
        # and keeps group 1/2's higher block numbers from being (incorrectly
        # and out-of-bounds) written into group 0's own bitmap block.
        self._write_bitmap(BLOCK_BITMAP_BLOCK, self.used_blocks, BLOCKS_PER_GROUP)

        # Group 0's inode bitmap: inode N is bit N-1 (1-based inode numbers,
        # 0-indexed bitmap) — again unchanged from every previous version;
        # max_bits=INODES_PER_GROUP already excludes group 1/2's inodes.
        inode_bits = {i - 1 for i in self.used_inodes}
        self._write_bitmap(INODE_BITMAP_BLOCK, inode_bits, INODES_PER_GROUP)

        # Groups 1..NUM_GROUPS-1: real, group-relative bitmaps (bit i ↔
        # absolute block/inode group_first_block(g)+i — the same convention
        # fs/ext2/alloc.c's kernel-side allocator already assumes for every
        # group). These groups didn't exist before this file supported
        # multiple groups, so there's no prior byte layout to preserve here.
        for g in range(1, NUM_GROUPS):
            base = group_first_block(g)
            bbitmap_blk, ibitmap_blk, _ = group_meta_blocks(g)
            group_block_bits = {
                blk - base for blk in self.used_blocks
                if base <= blk < base + BLOCKS_PER_GROUP
            }
            self._write_bitmap(bbitmap_blk, group_block_bits, BLOCKS_PER_GROUP)

            ino_base = g * INODES_PER_GROUP
            group_inode_bits = {
                (i - 1) - ino_base for i in self.used_inodes
                if ino_base < i <= ino_base + INODES_PER_GROUP
            }
            self._write_bitmap(ibitmap_blk, group_inode_bits, INODES_PER_GROUP)

        self._write_superblock()
        self._write_bgdt()

        with open(out_path, 'wb') as f:
            f.write(self.image)

        total_kb = TOTAL_BLOCKS * BLOCK_SIZE // 1024
        print(f"created {out_path}: EXT2 {total_kb} KiB, "
              f"{self.next_block - FIRST_FREE_BLOCK} data blocks used, "
              f"{self.next_inode - ROOT_INO - 1} user inodes")


# Short-name aliases for docs files, mirrored from tools/mkfat16.py so both
# filesystems present the same lookup names under /docs and /fat/docs.
# The applet set enabled in third_party/busybox/pureunix.config — kept in
# sync by hand (there's no way to introspect a prebuilt busybox.elf's
# applet table from Python without running it). See docs/userland.md's
# "BusyBox" section and tools/build-busybox.sh.
BUSYBOX_APPLETS = [
    "ash", "sh", "basename", "cat", "chmod", "chown", "clear", "cmp", "cp",
    "cut", "dd", "diff", "dirname", "echo", "egrep", "env", "false", "fgrep",
    "find", "grep", "head", "hexdump", "kill", "less", "ln", "ls", "mkdir",
    "mv", "nice", "od", "paste", "printenv", "printf", "ps", "pwd",
    "realpath", "renice", "rm", "rmdir", "seq", "sleep", "sort", "strings",
    "tail", "tee", "test", "top", "touch", "tr", "true", "uniq", "wc",
    "which", "xargs", "yes",
]

DOC_ALIASES = {
    "architecture.md": "arch.md",
    "boot.md":         "boot.md",
    "memory.md":       "memory.md",
    "interrupts.md":   "intrs.md",
    "scheduler.md":    "sched.md",
    "filesystem.md":   "fs.md",
    "syscalls.md":     "syscall.md",
    "drivers.md":      "drivers.md",
    "shell.md":        "shell.md",
    "userland.md":     "userland.md",
    "build.md":        "build.md",
    "developer-guide.md": "devguide.md",
    "project-layout.md":  "layout.md",
    # api/ subdirectory
    "README.md":  "readme.md",
    "task.md":    "task.md",
    "vfs.md":     "vfs.md",
    "libc.md":    "libc.md",
}


def add_docs(fs, docs_dir):
    """Add the docs/ tree to /docs on the EXT2 image."""
    docs_ino = fs.mkdir(ROOT_INO, 'docs')

    index_lines = [b'PureUnix documentation\n', b'cat /docs/<file> to read\n', b'\n']

    for fname in sorted(os.listdir(docs_dir)):
        fpath = os.path.join(docs_dir, fname)
        if os.path.isfile(fpath) and fname.endswith('.md'):
            short = DOC_ALIASES.get(fname, fname.lower())
            with open(fpath, 'rb') as f:
                fs.add_file(docs_ino, short, f.read())
            index_lines.append(f'  {short}\n'.encode())

    api_src = os.path.join(docs_dir, 'api')
    if os.path.isdir(api_src):
        api_ino = fs.mkdir(docs_ino, 'api')
        for fname in sorted(os.listdir(api_src)):
            fpath = os.path.join(api_src, fname)
            if os.path.isfile(fpath) and fname.endswith('.md'):
                short = DOC_ALIASES.get(fname, fname.lower())
                with open(fpath, 'rb') as f:
                    fs.add_file(api_ino, short, f.read())
                index_lines.append(f'  api/{short}\n'.encode())

    fs.add_file(docs_ino, 'index.txt', b''.join(index_lines))


EXEC_MODE = S_IFREG | 0o755   # rwxr-xr-x — programs need X_OK to run (Stage 3A)


def add_bin(fs, programs, dir_cache: dict):
    """Add the ELF program store to /bin on the EXT2 image."""
    bin_ino = ensure_dir(fs, dir_cache, '/bin')
    for program in programs:
        name = os.path.basename(program).lower()
        with open(program, 'rb') as f:
            fs.add_file(bin_ino, name, f.read(), mode=EXEC_MODE)

    # Stage 4: a symlink to an ELF, exercised by ext2test's "exec through a
    # symlink" regression check ("/bin/hello -> hello.elf must execute
    # correctly. Permission checks happen on the resolved inode."). The
    # symlink itself carries no execute bit at all (conventionally 0777,
    # but permission bits on a symlink are never consulted by anyone) —
    # elf_exec()'s X_OK check runs against hello.elf's own resolved mode.
    if any(os.path.basename(p).lower() == 'hello.elf' for p in programs):
        fs.add_symlink(bin_ino, 'hello', 'hello.elf')

    # Same idea for neatvi — a real standalone ELF (not a BusyBox applet),
    # so it just needs a plain name-without-.elf symlink to be reachable as
    # an ordinary PATH command ("neatvi"), same as any real installed editor.
    if any(os.path.basename(p).lower() == 'neatvi.elf' for p in programs):
        fs.add_symlink(bin_ino, 'neatvi', 'neatvi.elf')

    # Same idea for ping (user/ping.c, a thin CLI over SYS_PING -- see
    # docs/networking.md) -- a plain name-without-.elf symlink so it's
    # reachable as an ordinary PATH command ("ping 1.1.1.1"), not
    # "ping.elf 1.1.1.1".
    if any(os.path.basename(p).lower() == 'ping.elf' for p in programs):
        fs.add_symlink(bin_ino, 'ping', 'ping.elf')

    # Same idea for tty (user/tty.c -- ioctl(VT_GETACTIVE)/ioctl(VT_ACTIVATE)
    # over kernel/vt.c, see docs/vt.md) -- a plain name-without-.elf symlink
    # so BusyBox ash's own PATH lookup (an exact filename match, unlike the
    # legacy in-kernel shell's automatic ".elf" fallback) finds it as
    # "tty"/"tty N", not just "tty.elf".
    if any(os.path.basename(p).lower() == 'tty.elf' for p in programs):
        fs.add_symlink(bin_ino, 'tty', 'tty.elf')

    # BusyBox multi-call binary: one ELF, dispatched by argv[0]'s basename
    # (applets/applets.c's find_applet_by_name()) — every applet name is
    # just a symlink to the same busybox.elf, exactly like a real BusyBox
    # install's /bin/ls -> busybox. See docs/userland.md's "BusyBox" section.
    if any(os.path.basename(p).lower() == 'busybox.elf' for p in programs):
        for applet in BUSYBOX_APPLETS:
            fs.add_symlink(bin_ino, applet, 'busybox.elf')

    # Same idea for Lua/luac (third_party/lua/, docs/lua-port.md) -- plain
    # name-without-.elf symlinks so `lua`/`luac` work as ordinary PATH
    # commands from BusyBox ash, matching every other standalone ELF above.
    if any(os.path.basename(p).lower() == 'lua.elf' for p in programs):
        fs.add_symlink(bin_ino, 'lua', 'lua.elf')
    if any(os.path.basename(p).lower() == 'luac.elf' for p in programs):
        fs.add_symlink(bin_ino, 'luac', 'luac.elf')

    # Same idea for the SQLite CLI (third_party/sqlite/, docs/sqlite-port.md)
    # -- a plain name-without-.elf symlink so `sqlite3` works as an ordinary
    # PATH command.
    if any(os.path.basename(p).lower() == 'sqlite3.elf' for p in programs):
        fs.add_symlink(bin_ino, 'sqlite3', 'sqlite3.elf')

    # Same idea for the ncurses demo (third_party/ncurses/, docs/ncurses-
    # port.md) -- a plain name-without-.elf symlink so `ncdemo` works as an
    # ordinary PATH command.
    if any(os.path.basename(p).lower() == 'ncdemo.elf' for p in programs):
        fs.add_symlink(bin_ino, 'ncdemo', 'ncdemo.elf')

    # Same idea for htop (third_party/htop/, docs/htop-port.md) -- a plain
    # name-without-.elf symlink so `htop` works as an ordinary PATH command.
    if any(os.path.basename(p).lower() == 'htop.elf' for p in programs):
        fs.add_symlink(bin_ino, 'htop', 'htop.elf')

    # Same idea for Chocolate Doom (third_party/chocolate-doom/,
    # docs/chocolate-doom-port.md) -- a plain name-without-.elf symlink so
    # `chocolate-doom` works as an ordinary PATH command with no ".elf" to
    # type, matching the task's "type the command directly, no PATH setup"
    # requirement.
    if any(os.path.basename(p).lower() == 'chocolate-doom.elf' for p in programs):
        fs.add_symlink(bin_ino, 'chocolate-doom', 'chocolate-doom.elf')

    # Same idea for imgview (docs/imgview.md, real libpng/zlib-backed PNG
    # viewer) -- a plain name-without-.elf symlink so `imgview file.png`
    # works as an ordinary PATH command with no setup.
    if any(os.path.basename(p).lower() == 'imgview.elf' for p in programs):
        fs.add_symlink(bin_ino, 'imgview', 'imgview.elf')


def add_dev(fs, dir_cache: dict, num_vts: int = 6):
    """Add /dev/tty1../ttyN and /dev/tty -- see include/pureunix/vt.h and
    arch/i386/syscall.c's SYS_OPEN /dev/tty interception, which recognizes
    these paths and binds the resulting fd straight to a kernel/vt.c VT
    instead of ever reading these inodes' (empty) on-disk content. They
    exist on disk only so the paths are real, listable, stat()-able
    filesystem entries (`ls /dev`, `stat /dev/tty2`) -- PureUNIX's VFS has
    no true device-node/rdev machinery yet (see docs/vt.md), so S_IFCHR
    here is cosmetic: it makes `ls -l` show the right type letter, nothing
    more. Kept in sync with kernel/vt.c's NUM_VTS by the caller."""
    dev_ino = ensure_dir(fs, dir_cache, '/dev')
    mode = S_IFCHR | 0o666
    for n in range(1, num_vts + 1):
        fs.add_file(dev_ino, f'tty{n}', b'', mode=mode)
    fs.add_file(dev_ino, 'tty', b'', mode=mode)


def ensure_dir(fs, dir_cache: dict, path: str) -> int:
    """Return the inode for an absolute directory path, creating any
    missing components along the way (like `mkdir -p`) and caching every
    inode created so a later call for a path sharing a prefix (e.g. both
    /lib/tcc/include and /lib/tcc/lib under /lib/tcc) reuses it instead of
    creating the same directory twice.

    Also checks the real directory-entry list of an already-built parent
    (fs._dirs), not just dir_cache, before creating -- dir_cache only ever
    sees paths *this function* has created; several well-known top-level
    directories (/root, /home, /etc, ...) are created directly via
    fs.mkdir() elsewhere in main() and never registered in dir_cache, so
    without this check ensure_dir() would silently create a second,
    same-named-but-different-inode directory entry that shadows the real
    one (found the hard way: --extra-file /root/doom1.wad landed in an
    orphaned duplicate "root" directory, invisible at the real /root
    path — see docs/chocolate-doom-port.md's Testing section)."""
    path = path.strip('/')
    if not path:
        return ROOT_INO
    if path in dir_cache:
        return dir_cache[path]
    parent_path, _, name = path.rpartition('/')
    parent_ino = ensure_dir(fs, dir_cache, parent_path) if parent_path else ROOT_INO
    for entry_name, entry_ino, entry_ft in fs._dirs[parent_ino]:
        if entry_name == name and entry_ft == FT_DIR:
            dir_cache[path] = entry_ino
            return entry_ino
    ino = fs.mkdir(parent_ino, name)
    dir_cache[path] = ino
    return ino


def add_persistent_boot_files(fs, dir_cache: dict, kernel_elf_path: str):
    """Embeds /boot/pureunix.elf and a real /boot/grub/grub.cfg into the
    image, for the persistent-disk build (tools/mkdiskimg.py) — GRUB's own
    ext2 module reads both straight off this filesystem after the MBR/
    core.img handoff, with no multiboot2 modules declared (unlike
    boot/grub.cfg's ISO+ramdisk menuentry): the kernel finds and mounts this
    same partition as / itself once it's running (kernel_main()'s
    find_persistent_root_disk(), kernel/main.c) — no separate root.img
    ramdisk. Opt-in (only called when --persistent-boot is passed) so the
    ordinary $(DISK2)/ext2.img build used by the existing ISO+ramdisk boot
    path (boot/grub.cfg, make run/run-test) is completely unaffected."""
    boot_ino = ensure_dir(fs, dir_cache, 'boot')
    with open(kernel_elf_path, 'rb') as f:
        fs.add_file(boot_ino, 'pureunix.elf', f.read())

    grub_ino = ensure_dir(fs, dir_cache, 'boot/grub')
    fs.add_file(grub_ino, 'grub.cfg',
        b'set timeout=1\n'
        b'set default=0\n'
        b'\n'
        b'menuentry "PureUnix" {\n'
        b'    multiboot2 /boot/pureunix.elf\n'
        b'    boot\n'
        b'}\n')


def add_tree(fs, dir_cache: dict, dest_path: str, host_dir: str):
    """Recursively install a host directory tree onto the image at
    dest_path (an absolute path), preserving its directory structure —
    used for the TinyCC sysroot's include/ trees (docs/tcc-port.md), which
    unlike every other install helper here (add_docs/add_bin, both flat)
    are nested and too large to enumerate by hand."""
    dest_ino = ensure_dir(fs, dir_cache, dest_path)
    for entry in sorted(os.listdir(host_dir)):
        host_path = os.path.join(host_dir, entry)
        if entry == '.stamp':
            continue
        if os.path.isdir(host_path):
            add_tree(fs, dir_cache, dest_path + '/' + entry, host_path)
        else:
            with open(host_path, 'rb') as f:
                fs.add_file(dest_ino, entry, f.read())


def add_tcc(fs, dir_cache: dict, tcc_elf: str, tcc_sysroot: str):
    """Install the TinyCC compiler and its target sysroot (docs/tcc-port.md):
    /bin/tcc, TCC's own compiler-intrinsic headers at /lib/tcc/include,
    PureUNIX's newlib shadow headers at /usr/include/pureunix-compat,
    newlib's own headers at /usr/include, crt1.o/crti.o/crtn.o at
    /lib/tcc/lib (CONFIG_TCC_CRTPREFIX), libc.a/libm.a at /usr/lib, and
    libtcc1.a directly at /lib/tcc/libtcc1.a -- *not* under lib/, because
    TCC's own TCC_LIBTCC1 default (tcc.h) is the bare filename "libtcc1.a"
    resolved straight against CONFIG_TCCDIR ("/lib/tcc"), a different
    convention than CONFIG_TCC_CRTPREFIX/CONFIG_TCC_LIBPATHS use for
    everything else. Matches the paths baked into the Makefile's
    CONFIG_TCC_* -D flags, so a bare `tcc hello.c` finds everything with no
    extra configuration."""
    bin_ino = ensure_dir(fs, dir_cache, '/bin')
    with open(tcc_elf, 'rb') as f:
        fs.add_file(bin_ino, 'tcc', f.read(), mode=EXEC_MODE)

    add_tree(fs, dir_cache, '/lib/tcc/include', os.path.join(tcc_sysroot, 'include'))
    add_tree(fs, dir_cache, '/usr/include/pureunix-compat', os.path.join(tcc_sysroot, 'compat-include'))
    add_tree(fs, dir_cache, '/usr/include', os.path.join(tcc_sysroot, 'usr-include'))

    lib_dir = os.path.join(tcc_sysroot, 'lib')
    tcc_dir_ino = ensure_dir(fs, dir_cache, '/lib/tcc')
    tcc_lib_ino = ensure_dir(fs, dir_cache, '/lib/tcc/lib')
    usr_lib_ino = ensure_dir(fs, dir_cache, '/usr/lib')
    with open(os.path.join(lib_dir, 'libtcc1.a'), 'rb') as f:
        fs.add_file(tcc_dir_ino, 'libtcc1.a', f.read())
    for name in ('crt1.o', 'crti.o', 'crtn.o'):
        with open(os.path.join(lib_dir, name), 'rb') as f:
            fs.add_file(tcc_lib_ino, name, f.read())
    for name in ('libc.a', 'libm.a'):
        with open(os.path.join(lib_dir, name), 'rb') as f:
            fs.add_file(usr_lib_ino, name, f.read())


def add_lua(fs, dir_cache: dict):
    """Create Lua's default module search directories (luaconf.h's
    LUA_LDIR/LUA_CDIR, unmodified upstream default: LUA_ROOT "/usr/local/"
    -> /usr/local/share/lua/5.4/ for pure-Lua modules, /usr/local/lib/lua/
    5.4/ for C modules -- see docs/lua-port.md) so `require("modname")`
    finds real on-disk modules with zero LUA_PATH/LUA_CPATH configuration,
    the same way a conventional Unix Lua install works. Seeded with one
    real, tiny pure-Lua module (greet.lua) so require() has something
    genuine to find and the whole chain (package.path search, loadfile,
    dofile-as-part-of-require) is exercised by simply booting the image,
    not just by a test script written after the fact. Also creates /tmp
    (newlib's P_tmpdir, stdio.h) -- needed by os.tmpname()/tmpfile(),
    unrelated to Lua specifically but nothing created it before this."""
    lua_ldir_ino = ensure_dir(fs, dir_cache, '/usr/local/share/lua/5.4')
    ensure_dir(fs, dir_cache, '/usr/local/lib/lua/5.4')
    ensure_dir(fs, dir_cache, '/tmp')
    fs.add_file(lua_ldir_ino, 'greet.lua',
        b'-- A real installed Lua module (docs/lua-port.md) --\n'
        b"-- exercises require()'s on-disk package.path search.\n"
        b'local greet = {}\n\n'
        b'function greet.hello(name)\n'
        b'  return "Hello, " .. (name or "world") .. ", from a real Lua module!"\n'
        b'end\n\n'
        b'return greet\n')


def add_extra_file(fs, dir_cache: dict, host_path: str, dest_path: str):
    """Copies one arbitrary host file onto the image at an arbitrary
    absolute path, creating any missing parent directories -- a generic
    escape hatch for content that isn't a program (add_bin) or a doc
    (add_docs), e.g. an IWAD placed for Chocolate Doom testing
    (docs/chocolate-doom-port.md's Testing section: "an IWAD ... on the
    persistent filesystem must remain available across reboot" --
    verified by placing one this way, not by hand-editing the image)."""
    dest_path = dest_path.strip('/')
    parent, _, name = dest_path.rpartition('/')
    parent_ino = ensure_dir(fs, dir_cache, '/' + parent) if parent else ROOT_INO
    with open(host_path, 'rb') as f:
        fs.add_file(parent_ino, name, f.read())


def main(argv):
    if len(argv) < 2:
        print("usage: mkext2.py OUT.img [--docs DIR] [--tcc-elf PATH --tcc-sysroot DIR] "
              "[--persistent-boot KERNEL.elf] [--extra-file HOST:DEST ...] [program.elf ...]",
              file=sys.stderr)
        return 2

    out = argv[1]
    rest = argv[2:]

    docs_dir = None
    tcc_elf = None
    tcc_sysroot = None
    persistent_boot_kernel = None
    extra_files = []  # list of (host_path, dest_path)
    programs = []
    i = 0
    while i < len(rest):
        if rest[i] == '--docs' and i + 1 < len(rest):
            docs_dir = rest[i + 1]
            i += 2
        elif rest[i] == '--tcc-elf' and i + 1 < len(rest):
            tcc_elf = rest[i + 1]
            i += 2
        elif rest[i] == '--tcc-sysroot' and i + 1 < len(rest):
            tcc_sysroot = rest[i + 1]
            i += 2
        elif rest[i] == '--persistent-boot' and i + 1 < len(rest):
            persistent_boot_kernel = rest[i + 1]
            i += 2
        elif rest[i] == '--extra-file' and i + 1 < len(rest):
            host_path, _, dest_path = rest[i + 1].partition(':')
            extra_files.append((host_path, dest_path))
            i += 2
        else:
            programs.append(rest[i])
            i += 1

    fs  = Ext2Builder(volume_label='PUREUNIX_ROOT' if persistent_boot_kernel else 'PureUnixEXT2')
    dir_cache = {}   # absolute path -> inode, shared by ensure_dir() callers
                      # (add_bin/add_tcc) so e.g. /bin is only created once

    # ------------------------------------------------------------------ root
    fs.add_file(ROOT_INO, 'README.TXT',
        b'PureUnix EXT2 root filesystem\r\n'
        b'This file is served from the EXT2 data disk (ATA slave, ata1).\r\n'
        b'EXT2 is the primary root filesystem: /, /bin, /docs, /etc all live here.\r\n'
        b'FAT16 (ATA master, ata0) is mounted read/write at /fat for compatibility.\r\n'
        b'Type: cat /README.TXT  to verify EXT2 is serving the root VFS.\r\n')

    # ------------------------------------------------------------------ /etc
    etc_ino = fs.mkdir(ROOT_INO, 'etc')

    fs.add_file(etc_ino, 'passwd',
        b'root:x:0:0:root:/root:/bin/sh\n'
        b'guest:x:1000:1000:Guest:/home/guest:/bin/sh\n')

    fs.add_file(etc_ino, 'hostname',
        b'pureunix\n')

    # Stage 2D: a real symlink inode, to exercise S_ISLNK()/ls -l "l"
    # recognition end to end. Stage 4 makes path resolution actually follow
    # it — `cat readme.link` now transparently reads README.TXT.
    fs.add_symlink(ROOT_INO, 'readme.link', 'README.TXT')

    # Stage 4: an absolute-target symlink, alongside readme.link's
    # relative target, so ext2test can exercise both forms of readlink()
    # and path-resolution symlink-following.
    fs.add_symlink(ROOT_INO, 'abslink', '/README.TXT')

    # Stage 4: a symlink loop (A -> B -> A) for ELOOP detection — following
    # either endpoint must give up after 40 hops rather than recursing
    # forever.
    fs.add_symlink(ROOT_INO, 'loop_a', 'loop_b')
    fs.add_symlink(ROOT_INO, 'loop_b', 'loop_a')

    # ------------------------------------------------------------------ /dev
    add_dev(fs, dir_cache)

    # ------------------------------------------------------------------ /bin
    if programs:
        add_bin(fs, programs, dir_cache)

    # ------------------------------------------------------------- TinyCC
    if tcc_elf and tcc_sysroot:
        add_tcc(fs, dir_cache, tcc_elf, tcc_sysroot)

    # --------------------------------------------------------------- Lua
    add_lua(fs, dir_cache)

    # ------------------------------------------------------------------ /docs
    if docs_dir and os.path.isdir(docs_dir):
        add_docs(fs, docs_dir)

    # ------------------------------------------------------------------ /home
    home_ino = fs.mkdir(ROOT_INO, 'home')
    user_ino = fs.mkdir(home_ino, 'user')
    fs.mkdir(home_ino, 'guest')

    fs.add_file(user_ino, 'notes.txt',
        b'EXT2 nested directory test file.\n'
        b'If you can read this, path traversal works!\n')

    # /root -- /etc/passwd's root:...:/root:/bin/sh already names this as
    # root's home directory, and kernel/main.c's auto-login path sets
    # $HOME=/root and calls shell_set_home_cwd("/root") on every boot, but
    # nothing ever created the directory itself -- shell_set_home_cwd()
    # silently no-ops if vfs_stat() doesn't find a real directory there
    # (shell/sh.c), so ash was actually starting in / the whole time, not
    # /root as HOME/the password database both claim. Found while testing
    # the Lua port (writing a script to "$HOME/test.lua" failed with
    # "nonexistent directory") but this is a pre-existing gap unrelated to
    # Lua specifically -- fixing it here so every account's home directory
    # genuinely exists, matching /etc/passwd.
    fs.mkdir(ROOT_INO, 'root')

    # ----------------------------------------------------------------- /testdir (for readdir test)
    testdir_ino = fs.mkdir(ROOT_INO, 'testdir')
    for name in ['alpha.txt', 'beta.txt', 'gamma.txt']:
        fs.add_file(testdir_ino, name, f'Content of {name}\n'.encode())

    # Stage 4: a relative symlink target using "..", resolved from the
    # symlink's own parent directory (/testdir) exactly like Unix — this
    # must land on /README.TXT, not /testdir/README.TXT.
    fs.add_symlink(testdir_ino, 'uplink', '../README.TXT')

    # ------------------------------------------------------------------ bigfile.bin
    # 5 blocks, all direct — tests multi-block direct reads. Sized off
    # BLOCK_SIZE (not a hardcoded byte count) so this scales correctly
    # whether the image uses 1 KB or 4 KB blocks.
    big_data = b'B' * (5 * BLOCK_SIZE)
    fs.add_file(ROOT_INO, 'bigfile.bin', big_data)

    # ------------------------------------------------------------------ hugefile.bin
    # A file that exceeds 12 direct blocks and requires the singly-indirect
    # block pointer (i_block[12]) — 14 blocks → first 12 are direct, last 2
    # via singly-indirect. Also sized off BLOCK_SIZE.
    huge_size = 14 * BLOCK_SIZE
    huge_data = bytes(range(256)) * (huge_size // 256)
    assert len(huge_data) == huge_size
    fs.add_file(ROOT_INO, 'hugefile.bin', huge_data)

    # ------------------------------------------------------------------ Stage 3A permission fixtures
    # One asset per mode called out by the Stage 3A regression suite, plus a
    # group-vs-other case (group_test.txt) so the permission engine's three
    # tiers (owner/group/other) are all exercised, not just owner-vs-other.
    perm_ino = fs.mkdir(ROOT_INO, 'perm')

    fs.add_file(perm_ino, 'exec.sh',
        b'#!/bin/sh\necho this file exists only to carry an executable bit\n',
        mode=S_IFREG | 0o755, uid=0, gid=0)

    fs.add_file(perm_ino, 'private.txt',
        b'owner-only: root can read this via the uid-0 bypass; a non-root\n'
        b'uid with no matching owner/group must be denied (other bits are 0).\n',
        mode=S_IFREG | 0o600, uid=0, gid=0)

    fs.add_file(perm_ino, 'readonly.txt',
        b'world-readable, nobody-writable except root (root write always\n'
        b'succeeds regardless of mode bits).\n',
        mode=S_IFREG | 0o444, uid=0, gid=0)

    fs.add_file(perm_ino, 'noaccess.bin',
        b'\x00' * 16,
        mode=S_IFREG | 0o000, uid=0, gid=0)

    fs.add_file(perm_ino, 'group_test.txt',
        b'owned by uid=0, gid=100: group members can read but not write;\n'
        b'anyone else falls through to the (all-zero) other bits.\n',
        mode=S_IFREG | 0o640, uid=0, gid=100)

    fs.mkdir(perm_ino, 'emptydir')  # directory with nothing but '.' and '..'

    # A directory with no execute bit anywhere (0600, not even for root — the
    # permission engine's root exception for X_OK requires *some* x bit to be
    # set), so a path through it exercises ancestor X_OK traversal denial.
    # The file inside is unreachable by design; its content is never checked.
    noxdir_ino = fs.mkdir(perm_ino, 'noxdir', mode=S_IFDIR | 0o600)
    fs.add_file(noxdir_ino, 'hidden.txt', b'unreachable\n', mode=S_IFREG | 0o644, uid=0, gid=0)

    # ------------------------------------------------------------ /tcctests
    # TinyCC port regression fixtures (docs/tcc-port.md) -- small C sources
    # exercising the "hello world", multi-file link, and syscall-using-program
    # rungs of the self-compilation ladder, permanently on the image the same
    # way bigfile.bin/hugefile.bin/perm/ are: so `tcc` can be smoke-tested
    # after any kernel/libc change (bare `tcc hello.c && ./a.out` on a real
    # source tree, not just a typed-in one-liner) without needing network
    # access or typing C source through the emulated keyboard by hand.
    if tcc_elf and tcc_sysroot:
        tcctests_ino = fs.mkdir(ROOT_INO, 'tcctests')

        fs.add_file(tcctests_ino, 'hello.c',
            b'#include <stdio.h>\n'
            b'int main(void) {\n'
            b'    printf("hello from tcc\\n");\n'
            b'    return 0;\n'
            b'}\n')

        fs.add_file(tcctests_ino, 'cat.c',
            b'#include <stdio.h>\n'
            b'#include <fcntl.h>\n'
            b'#include <unistd.h>\n'
            b'int main(int argc, char *argv[]) {\n'
            b'    if (argc < 2) { return 1; }\n'
            b'    int fd = open(argv[1], O_RDONLY);\n'
            b'    if (fd < 0) { return 2; }\n'
            b'    char buf[128];\n'
            b'    int n;\n'
            b'    while ((n = read(fd, buf, sizeof(buf))) > 0) {\n'
            b'        write(1, buf, n);\n'
            b'    }\n'
            b'    close(fd);\n'
            b'    return 0;\n'
            b'}\n')

        fs.add_file(tcctests_ino, 'grep.c',
            b'#include <stdio.h>\n'
            b'#include <string.h>\n'
            b'int main(int argc, char *argv[]) {\n'
            b'    if (argc < 3) { return 1; }\n'
            b'    FILE *f = fopen(argv[2], "r");\n'
            b'    if (!f) { return 2; }\n'
            b'    char line[256];\n'
            b'    while (fgets(line, sizeof(line), f)) {\n'
            b'        if (strstr(line, argv[1])) { fputs(line, stdout); }\n'
            b'    }\n'
            b'    fclose(f);\n'
            b'    return 0;\n'
            b'}\n')

        fs.add_file(tcctests_ino, 'helper.h',
            b'int add_numbers(int a, int b);\n')

        fs.add_file(tcctests_ino, 'helper.c',
            b'#include "helper.h"\n'
            b'int add_numbers(int a, int b) {\n'
            b'    return a + b;\n'
            b'}\n')

        fs.add_file(tcctests_ino, 'mainc.c',
            b'#include <stdio.h>\n'
            b'#include "helper.h"\n'
            b'int main(void) {\n'
            b'    printf("sum is %d\\n", add_numbers(19, 23));\n'
            b'    return 0;\n'
            b'}\n')

        fs.add_file(tcctests_ino, 'greeting.txt',
            b'first line here\n'
            b'needle line has target text\n'
            b'last line here\n')

    # ---------------------------------------------------------- /boot (persistent disk only)
    if persistent_boot_kernel:
        add_persistent_boot_files(fs, dir_cache, persistent_boot_kernel)

    # ------------------------------------------------------------ --extra-file
    for host_path, dest_path in extra_files:
        add_extra_file(fs, dir_cache, host_path, dest_path)

    # ------------------------------------------------------------------ build
    fs.build(out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
