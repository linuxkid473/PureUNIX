#!/usr/bin/env python3
"""
mkext2.py — minimal EXT2 filesystem image builder for PureUNIX.

Creates a read-only EXT2 image suitable for testing the Stage 1 EXT2
driver.  Only the structures needed for reading are populated; journal,
extended attributes, and resize metadata are not used.

Layout (1 KB blocks, single block group):
  Block 0  : boot block / padding (first 1024 bytes, before the superblock)
  Block 1  : superblock
  Block 2  : block group descriptor table (BGDT)
  Block 3  : block bitmap
  Block 4  : inode bitmap
  Block 5  : inode table (128 blocks for 1024 inodes × 128 bytes/inode)
   ...
  Block 133: first data block
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
TOTAL_BLOCKS      = 8192          # 8 MB image — bumped from 4 MB (still the
                                   # max a single block group supports at
                                   # this 1 KB block size, see BLOCKS_PER_GROUP
                                   # above) to fit the TinyCC port's sysroot
                                   # (crt objects, libtcc1.a, a merged libc.a,
                                   # and newlib's ~140-header tree — see
                                   # docs/tcc-port.md) alongside everything
                                   # already installed.
FIRST_DATA_BLOCK  = 1             # for 1 KB blocks the SB lives in block 1

# Block layout for group 0
BGDT_BLOCK        = 2
BLOCK_BITMAP_BLOCK= 3
INODE_BITMAP_BLOCK= 4
INODE_TABLE_BLOCK = 5
INODE_TABLE_BLOCKS= (INODES_PER_GROUP * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE  # 128
FIRST_FREE_BLOCK  = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS  # 133

# EXT2 inode mode bits
S_IFDIR = 0x4000
S_IFREG = 0x8000
S_IFLNK = 0xA000
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
    def __init__(self):
        self.image        = bytearray(TOTAL_BLOCKS * BLOCK_SIZE)
        self.next_block   = FIRST_FREE_BLOCK
        self.next_inode   = ROOT_INO + 1   # inode 2 is root; 3+ are free
        # Track usage for bitmaps
        self.used_blocks  = set(range(FIRST_FREE_BLOCK))
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
        if b >= TOTAL_BLOCKS:
            raise RuntimeError("EXT2 image full")
        self.next_block += 1
        self.used_blocks.add(b)
        return b

    def _alloc_inode(self):
        i = self.next_inode
        if i > INODES_PER_GROUP:
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
            local_idx    = ino - 1
            byte_off     = local_idx * INODE_SIZE
            blk_off      = byte_off // BLOCK_SIZE
            off_in_blk   = byte_off % BLOCK_SIZE
            abs_blk      = INODE_TABLE_BLOCK + blk_off
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
        num_used_inodes  = len(self.used_inodes)
        num_used_blocks  = len(self.used_blocks)
        free_blocks      = TOTAL_BLOCKS - num_used_blocks
        free_inodes      = INODES_PER_GROUP - num_used_inodes
        used_dirs        = sum(1 for i in self._dirs)

        # Superblock is 1024 bytes; pad with zeros to fill
        sb = bytearray(BLOCK_SIZE)

        def put32(off, v): struct.pack_into('<I', sb, off, v & 0xFFFFFFFF)
        def put16(off, v): struct.pack_into('<H', sb, off, v & 0xFFFF)
        def put8 (off, v): sb[off] = v & 0xFF

        put32(0,   INODES_PER_GROUP)          # s_inodes_count
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
        # Volume name
        sb[120:136] = b'PureUnixEXT2\x00\x00\x00\x00'

        # Write to disk — the superblock occupies the SECOND 1 KB block
        # (byte offset 1024 = block 1 × 1024)
        start = FIRST_DATA_BLOCK * BLOCK_SIZE
        self.image[start : start + BLOCK_SIZE] = sb

    def _write_bgdt(self):
        num_free_blocks = TOTAL_BLOCKS - len(self.used_blocks)
        num_free_inodes = INODES_PER_GROUP - len(self.used_inodes)
        used_dirs       = len(self._dirs)

        # Single group descriptor (32 bytes)
        entry = struct.pack('<IIIHHHI',
            BLOCK_BITMAP_BLOCK,   # bg_block_bitmap
            INODE_BITMAP_BLOCK,   # bg_inode_bitmap
            INODE_TABLE_BLOCK,    # bg_inode_table
            num_free_blocks,      # bg_free_blocks_count
            num_free_inodes,      # bg_free_inodes_count
            used_dirs,            # bg_used_dirs_count
            0,                    # bg_pad + bg_reserved (combined as one I)
        )
        buf = entry + b'\x00' * (BLOCK_SIZE - len(entry))
        self._write_block(BGDT_BLOCK, buf)

    # ------------------------------------------------------------------
    # Finalise and emit
    # ------------------------------------------------------------------
    def build(self, out_path: str):
        self._finalise_dirs()
        self._write_inode_table()

        # Block bitmap: blocks 0..FIRST_FREE_BLOCK-1 are used, plus whatever
        # data blocks were allocated during finalise/file creation
        self._write_bitmap(BLOCK_BITMAP_BLOCK, self.used_blocks, TOTAL_BLOCKS)

        # Inode bitmap: inode N is bit N (1-based, but the bitmap is 0-indexed
        # from the start of the group, so inode 1 → bit 0)
        inode_bits = {i - 1 for i in self.used_inodes}
        self._write_bitmap(INODE_BITMAP_BLOCK, inode_bits, INODES_PER_GROUP)

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
    "mv", "od", "paste", "printenv", "printf", "pwd", "realpath", "rm",
    "rmdir", "seq", "sleep", "sort", "strings", "tail", "tee", "test",
    "touch", "tr", "true", "uniq", "wc", "which", "xargs", "yes",
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

    # BusyBox multi-call binary: one ELF, dispatched by argv[0]'s basename
    # (applets/applets.c's find_applet_by_name()) — every applet name is
    # just a symlink to the same busybox.elf, exactly like a real BusyBox
    # install's /bin/ls -> busybox. See docs/userland.md's "BusyBox" section.
    if any(os.path.basename(p).lower() == 'busybox.elf' for p in programs):
        for applet in BUSYBOX_APPLETS:
            fs.add_symlink(bin_ino, applet, 'busybox.elf')


def ensure_dir(fs, dir_cache: dict, path: str) -> int:
    """Return the inode for an absolute directory path, creating any
    missing components along the way (like `mkdir -p`) and caching every
    inode created so a later call for a path sharing a prefix (e.g. both
    /lib/tcc/include and /lib/tcc/lib under /lib/tcc) reuses it instead of
    creating the same directory twice."""
    path = path.strip('/')
    if not path:
        return ROOT_INO
    if path in dir_cache:
        return dir_cache[path]
    parent_path, _, name = path.rpartition('/')
    parent_ino = ensure_dir(fs, dir_cache, parent_path) if parent_path else ROOT_INO
    ino = fs.mkdir(parent_ino, name)
    dir_cache[path] = ino
    return ino


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


def main(argv):
    if len(argv) < 2:
        print("usage: mkext2.py OUT.img [--docs DIR] [--tcc-elf PATH --tcc-sysroot DIR] [program.elf ...]", file=sys.stderr)
        return 2

    out = argv[1]
    rest = argv[2:]

    docs_dir = None
    tcc_elf = None
    tcc_sysroot = None
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
        else:
            programs.append(rest[i])
            i += 1

    fs  = Ext2Builder()
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

    # ------------------------------------------------------------------ /bin
    if programs:
        add_bin(fs, programs, dir_cache)

    # ------------------------------------------------------------- TinyCC
    if tcc_elf and tcc_sysroot:
        add_tcc(fs, dir_cache, tcc_elf, tcc_sysroot)

    # ------------------------------------------------------------------ /docs
    if docs_dir and os.path.isdir(docs_dir):
        add_docs(fs, docs_dir)

    # ------------------------------------------------------------------ /home
    home_ino = fs.mkdir(ROOT_INO, 'home')
    user_ino = fs.mkdir(home_ino, 'user')

    fs.add_file(user_ino, 'notes.txt',
        b'EXT2 nested directory test file.\n'
        b'If you can read this, path traversal works!\n')

    # ----------------------------------------------------------------- /testdir (for readdir test)
    testdir_ino = fs.mkdir(ROOT_INO, 'testdir')
    for name in ['alpha.txt', 'beta.txt', 'gamma.txt']:
        fs.add_file(testdir_ino, name, f'Content of {name}\n'.encode())

    # Stage 4: a relative symlink target using "..", resolved from the
    # symlink's own parent directory (/testdir) exactly like Unix — this
    # must land on /README.TXT, not /testdir/README.TXT.
    fs.add_symlink(testdir_ino, 'uplink', '../README.TXT')

    # ------------------------------------------------------------------ bigfile.bin
    # 5 KB = 5 direct blocks — tests multi-block direct reads.
    big_data = b'B' * (5 * BLOCK_SIZE)   # 5120 bytes, all 0x42
    assert len(big_data) == 5120
    fs.add_file(ROOT_INO, 'bigfile.bin', big_data)

    # ------------------------------------------------------------------ hugefile.bin
    # A file that exceeds 12 direct blocks (>12 KB) and requires the
    # singly-indirect block pointer (i_block[12]).
    # 14 KB = 14 blocks → first 12 are direct, last 2 via singly-indirect.
    huge_size = 14 * BLOCK_SIZE   # 14336 bytes
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

    # ------------------------------------------------------------------ build
    fs.build(out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
