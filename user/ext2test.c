#include "libpure.h"

/* ------------------------------------------------------------------ helpers */

static void pass(const char *msg)
{
    pu_puts("  [PASS] ");
    pu_puts(msg);
    pu_puts("\n");
}

static void fail(const char *msg, int val)
{
    pu_puts("  [FAIL] ");
    pu_puts(msg);
    pu_puti(val);
    pu_puts("\n");
}

/* Compare n bytes; return 0 if equal, 1 if not. */
static int xmemcmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)a[i] != (unsigned char)b[i]) return 1;
    return 0;
}

/* Compare at most n bytes of two C strings; return 0 if equal. */
static int xstrncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/* Print at most n bytes, replacing non-printable chars with '.'. */
static void print_bytes(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c >= 32 && c < 127) pu_write(1, &c, 1);
        else                     pu_puts(".");
    }
}

/* Compare two C strings; return 1 if equal. */
static int streq(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
        i++;
    }
}

/* Find name among the first count entries; return its index or -1. */
static int find_name(struct dirent *entries, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (streq(entries[i].name, name)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    pu_puts("=== ext2test ===\n");

    struct stat st;
    char buf[256];
    int fd, n, r;

    /* ------------------------------------------------------------------ 1 */
    pu_puts("\n[1] stat root directory\n");

    r = pu_stat("/", &st);
    if (r == 0 && st.st_type == 2)
        pass("stat('/') returns type=DIR");
    else
        fail("expected VFS_DIR(2), got st_type=", st.st_type);

    /* ------------------------------------------------------------------ 2 */
    pu_puts("\n[2] stat /README.TXT (regular file)\n");

    r = pu_stat("/README.TXT", &st);
    if (r != 0) {
        fail("stat returned error: ", r);
    } else {
        if (st.st_type == 1)
            pass("stat('/README.TXT') type=FILE");
        else
            fail("expected FILE(1), got ", (int)st.st_type);

        if (st.st_size > 0) {
            pu_puts("  size="); pu_puti((int)st.st_size); pu_puts("\n");
            pass("stat('/README.TXT') size > 0");
        } else {
            fail("expected size>0, got ", (int)st.st_size);
        }
    }

    /* ------------------------------------------------------------------ 3 */
    pu_puts("\n[3] open + read /README.TXT, check content\n");

    fd = pu_open("/README.TXT", O_RDONLY);
    if (fd < 3) {
        fail("FATAL: cannot open /README.TXT, fd=", fd);
        return 1;
    }

    n = pu_read(fd, buf, 12);
    if (n == 12 && xstrncmp(buf, "PureUnix EXT", 12) == 0)
        pass("first 12 bytes of README.TXT match 'PureUnix EXT'");
    else {
        pu_puts("  got: ["); print_bytes(buf, n > 0 ? n : 0); pu_puts("]\n");
        fail("content mismatch, n=", n);
    }

    /* ------------------------------------------------------------------ 4 */
    pu_puts("\n[4] SEEK_SET 0 + re-read matches original\n");

    char buf2[12];
    pu_lseek(fd, 0, SEEK_SET);
    n = pu_read(fd, buf2, 12);
    if (n == 12 && xmemcmp(buf, buf2, 12) == 0)
        pass("re-read after SEEK_SET 0 matches");
    else
        fail("re-read mismatch, n=", n);

    /* ------------------------------------------------------------------ 5 */
    pu_puts("\n[5] EOF behaviour on /README.TXT\n");

    pu_lseek(fd, 0, SEEK_END);
    n = pu_read(fd, buf, 32);
    if (n == 0)
        pass("read at SEEK_END returns 0");
    else
        fail("expected 0 at EOF, got ", n);

    n = pu_read(fd, buf, 32);
    if (n == 0)
        pass("second read at EOF still returns 0");
    else
        fail("expected 0, got ", n);

    pu_close(fd);

    /* ------------------------------------------------------------------ 6 */
    pu_puts("\n[6] stat /etc — nested directory\n");

    r = pu_stat("/etc", &st);
    if (r == 0 && st.st_type == 2)
        pass("stat('/etc') returns type=DIR");
    else
        fail("expected VFS_DIR, got type=", (int)st.st_type);

    /* ------------------------------------------------------------------ 7 */
    pu_puts("\n[7] open + read /etc/passwd\n");

    fd = pu_open("/etc/passwd", O_RDONLY);
    if (fd < 3) {
        fail("cannot open /etc/passwd, fd=", fd);
    } else {
        n = pu_read(fd, buf, 10);
        if (n == 10 && xstrncmp(buf, "root:x:0:0", 10) == 0)
            pass("first 10 bytes of /etc/passwd match 'root:x:0:0'");
        else {
            pu_puts("  got: ["); print_bytes(buf, n > 0 ? n : 0); pu_puts("]\n");
            fail("/etc/passwd content mismatch, n=", n);
        }
        pu_close(fd);
    }

    /* ------------------------------------------------------------------ 8 */
    pu_puts("\n[8] deeply nested path: /home/user/notes.txt\n");

    fd = pu_open("/home/user/notes.txt", O_RDONLY);
    if (fd < 3) {
        fail("cannot open /home/user/notes.txt, fd=", fd);
    } else {
        n = pu_read(fd, buf, 4);
        if (n == 4 && xstrncmp(buf, "EXT2", 4) == 0)
            pass("read /home/user/notes.txt begins with 'EXT2'");
        else {
            pu_puts("  got: ["); print_bytes(buf, n > 0 ? n : 0); pu_puts("]\n");
            fail("content mismatch, n=", n);
        }
        pu_close(fd);
    }

    /* ------------------------------------------------------------------ 9 */
    pu_puts("\n[9] open nonexistent file -> ENOENT\n");

    fd = pu_open("/no/such/file.txt", O_RDONLY);
    if (fd == ENOENT)
        pass("open('/no/such/file.txt') returns ENOENT");
    else
        fail("expected ENOENT(-2), got ", fd);

    /* ----------------------------------------------------------------- 10 */
    pu_puts("\n[10] open a directory -> EISDIR\n");

    fd = pu_open("/etc", O_RDONLY);
    if (fd == EISDIR)
        pass("open('/etc') returns EISDIR");
    else if (fd >= 3) {
        pu_puts("  (driver opened directory fd — not an error in this kernel)\n");
        pu_close(fd);
        pass("open('/etc') opened directory fd (acceptable)");
    } else {
        fail("unexpected return from open('/etc'): ", fd);
    }

    /* ----------------------------------------------------------------- 11 */
    pu_puts("\n[11] bigfile.bin: 5 blocks (5120 bytes), all 'B'\n");

    r = pu_stat("/bigfile.bin", &st);
    if (r == 0 && st.st_size == 5120) {
        pass("stat('/bigfile.bin') size=5120");
    } else {
        pu_puts("  size="); pu_puti((int)st.st_size); pu_puts("\n");
        fail("expected size=5120, stat returned ", r);
    }

    fd = pu_open("/bigfile.bin", O_RDONLY);
    if (fd < 3) {
        fail("cannot open /bigfile.bin, fd=", fd);
    } else {
        /* Read across the block boundary: bytes 1020..1027 span blocks 0 and 1 */
        pu_lseek(fd, 1020, SEEK_SET);
        n = pu_read(fd, buf, 8);
        int all_B = 1;
        for (int i = 0; i < 8; i++)
            if (buf[i] != 'B') { all_B = 0; break; }
        if (n == 8 && all_B)
            pass("8 bytes spanning block boundary are all 'B'");
        else
            fail("block-boundary read wrong, n=", n);

        /* Seek to last block (block 4, offset 4096) and read till EOF */
        pu_lseek(fd, 4096, SEEK_SET);
        char last_blk[1024];
        n = pu_read(fd, last_blk, 1024);
        all_B = 1;
        for (int i = 0; i < n; i++)
            if (last_blk[i] != 'B') { all_B = 0; break; }
        if (n == 1024 && all_B)
            pass("last direct block (5th, offset 4096) reads 1024 'B's");
        else
            fail("last block read wrong, n=", n);

        /* One more read hits EOF */
        n = pu_read(fd, buf, 1);
        if (n == 0)
            pass("read after last block returns EOF");
        else
            fail("expected EOF after bigfile, got ", n);

        pu_close(fd);
    }

    /* ----------------------------------------------------------------- 12 */
    pu_puts("\n[12] hugefile.bin: 14 blocks (14336 bytes), needs singly-indirect\n");

    r = pu_stat("/hugefile.bin", &st);
    if (r == 0 && st.st_size == 14336) {
        pass("stat('/hugefile.bin') size=14336");
    } else {
        pu_puts("  size="); pu_puti((int)st.st_size); pu_puts("\n");
        fail("expected size=14336, stat returned ", r);
    }

    fd = pu_open("/hugefile.bin", O_RDONLY);
    if (fd < 3) {
        fail("cannot open /hugefile.bin, fd=", fd);
    } else {
        /* First byte of each block should be (block_offset % 256).
           block_size=1024; 1024%256=0, so every block starts with 0x00. */
        int indirect_ok = 1;
        for (int blk = 0; blk < 14; blk++) {
            pu_lseek(fd, blk * 1024, SEEK_SET);
            unsigned char c;
            n = pu_read(fd, (char *)&c, 1);
            if (n != 1 || c != 0x00) { indirect_ok = 0; break; }
        }
        if (indirect_ok)
            pass("all 14 blocks start with 0x00 (direct + singly-indirect)");
        else
            fail("block boundary byte wrong at block ", 0);

        /* Byte at offset 12*1024 = 12288 is the first byte of the
           singly-indirect region; should be 0x00 */
        pu_lseek(fd, 12 * 1024, SEEK_SET);
        {
            unsigned char c;
            n = pu_read(fd, (char *)&c, 1);
            if (n == 1 && c == 0x00)
                pass("first byte of singly-indirect block (offset 12288) = 0x00");
            else
                fail("indirect first byte wrong, n=", n);
        }

        /* Byte at offset 13*1024+255=13567 = last byte of block 13 (indirect) */
        pu_lseek(fd, 13 * 1024 + 255, SEEK_SET);
        {
            unsigned char c;
            n = pu_read(fd, (char *)&c, 1);
            if (n == 1 && c == 0xFF)
                pass("byte at offset 13*1024+255 = 0xFF (pattern verified)");
            else {
                pu_puts("  got 0x"); pu_puti((int)c); pu_puts("\n");
                fail("pattern byte wrong, n=", n);
            }
        }

        /* EOF after 14 blocks */
        pu_lseek(fd, 14 * 1024, SEEK_SET);
        n = pu_read(fd, buf, 1);
        if (n == 0)
            pass("read at offset 14336 returns EOF");
        else
            fail("expected EOF at end of hugefile, got ", n);

        pu_close(fd);
    }

    /* ----------------------------------------------------------------- 13 */
    pu_puts("\n[13] SEEK_CUR relative navigation in hugefile.bin\n");

    fd = pu_open("/hugefile.bin", O_RDONLY);
    if (fd < 3) {
        fail("cannot open /hugefile.bin for seek test, fd=", fd);
    } else {
        /* Seek to block 12 (singly-indirect zone), read 256 bytes */
        pu_lseek(fd, 12 * 1024, SEEK_SET);
        n = pu_read(fd, buf, 256);
        if (n == 256) {
            /* Verify sequential pattern 0x00..0xFF */
            int pat_ok = 1;
            for (int i = 0; i < 256; i++)
                if ((unsigned char)buf[i] != (unsigned char)i) { pat_ok = 0; break; }
            if (pat_ok)
                pass("256-byte read from block 12 (indirect) matches 0x00..0xFF");
            else
                fail("indirect block pattern mismatch at i=", 0);
        } else {
            fail("expected 256 bytes from block 12, got ", n);
        }

        /* Use SEEK_CUR to step back 128 bytes and re-read */
        int cur = pu_lseek(fd, -128, SEEK_CUR);
        if (cur == 12 * 1024 + 128) {
            pass("SEEK_CUR -128 lands at expected position");
        } else {
            pu_puts("  expected="); pu_puti(12 * 1024 + 128);
            pu_puts("  got="); pu_puti(cur); pu_puts("\n");
            fail("SEEK_CUR returned wrong position: ", cur);
        }

        char rebuf[128];
        n = pu_read(fd, rebuf, 128);
        if (n == 128 && xmemcmp(buf + 128, rebuf, 128) == 0)
            pass("re-read 128 bytes after SEEK_CUR back matches original");
        else
            fail("re-read mismatch, n=", n);

        pu_close(fd);
    }

    /* ----------------------------------------------------------------- 14 */
    pu_puts("\n[14] Two simultaneous fds on different EXT2 files\n");

    int fa = pu_open("/README.TXT",  O_RDONLY);
    int fb = pu_open("/etc/passwd",  O_RDONLY);

    if (fa >= 3 && fb >= 3) {
        pu_puts("  fa="); pu_puti(fa);
        pu_puts("  fb="); pu_puti(fb); pu_puts("\n");

        char a[8], b2[8];
        int na = pu_read(fa, a,  8);
        int nb = pu_read(fb, b2, 8);

        /* Files have different content — reads should not interfere */
        if (na == 8 && nb == 8 && xmemcmp(a, b2, 8) != 0)
            pass("two fds on different files return distinct content");
        else if (na != 8 || nb != 8)
            fail("short read, na+nb=", na + nb);
        else
            pass("two fds on different files (content happened to match — ok)");

        /* Seeking fa must not change fb's position */
        pu_lseek(fa, 0, SEEK_SET);
        int fb_pos = pu_lseek(fb, 0, SEEK_CUR);
        if (fb_pos == 8)
            pass("seeking fa does not affect fb offset");
        else
            fail("fb offset wrong, expected 8, got ", fb_pos);

        pu_close(fa);
        pu_close(fb);
    } else {
        pu_puts("  (skip: could not open two fds simultaneously)\n");
        if (fa >= 3) pu_close(fa);
        if (fb >= 3) pu_close(fb);
    }

    /* ----------------------------------------------------------------- 15 */
    pu_puts("\n[15] directory traversal: root listing\n");

    {
        struct dirent entries[48];
        int count = pu_readdir("/", entries, 48);
        if (count < 0) {
            fail("pu_readdir('/') returned error: ", count);
        } else {
            pu_puts("  entries="); pu_puti(count); pu_puts("\n");
            const char *expect[] = { "README.TXT", "etc", "bin", "docs", "home",
                                      "testdir", "perm", "readme.link",
                                      "bigfile.bin", "hugefile.bin" };
            int all_found = 1;
            for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
                if (find_name(entries, count, expect[i]) < 0) {
                    pu_puts("  missing: "); pu_puts(expect[i]); pu_puts("\n");
                    all_found = 0;
                }
            }
            if (all_found)
                pass("root listing contains every expected top-level entry");
            else
                fail("root listing missing expected entries, count=", count);
        }
    }

    /* ----------------------------------------------------------------- 16 */
    pu_puts("\n[16] directory traversal: nested directory listing (/testdir)\n");

    {
        struct dirent entries[16];
        int count = pu_readdir("/testdir", entries, 16);
        if (count < 0) {
            fail("pu_readdir('/testdir') returned error: ", count);
        } else if (find_name(entries, count, "alpha.txt") >= 0 &&
                   find_name(entries, count, "beta.txt")  >= 0 &&
                   find_name(entries, count, "gamma.txt") >= 0) {
            pass("nested listing /testdir contains alpha/beta/gamma.txt");
        } else {
            fail("nested listing missing expected files, count=", count);
        }
    }

    /* ----------------------------------------------------------------- 17 */
    pu_puts("\n[17] directory traversal: '.' and '..' entries\n");

    {
        /* The VFS never filters these out — see fs/ext2/dir.c's
           readdir_block — so they show up in every pu_readdir() result
           exactly as they would feed a shell's `ls -a`/`ls -la`. Nothing
           is fabricated here or in the kernel; this just confirms the
           driver-level entries Stage 2D added are still exposed. */
        struct dirent entries[48];
        int count = pu_readdir("/", entries, 48);
        if (count >= 0 && find_name(entries, count, ".") >= 0)
            pass("root listing includes '.' entry");
        else
            fail("root listing missing '.' entry, count=", count);

        if (count >= 0 && find_name(entries, count, "..") >= 0)
            pass("root listing includes '..' entry");
        else
            fail("root listing missing '..' entry, count=", count);

        struct dirent empty_entries[8];
        int empty_count = pu_readdir("/perm/emptydir", empty_entries, 8);
        if (empty_count == 2 &&
            find_name(empty_entries, empty_count, ".")  >= 0 &&
            find_name(empty_entries, empty_count, "..") >= 0) {
            pass("emptydir contains exactly '.' and '..', nothing fabricated");
        } else {
            fail("emptydir entry count wrong, got ", empty_count);
        }
    }

    /* ----------------------------------------------------------------- 18 */
    pu_puts("\n[18] directory sizes come from the real inode (not hardcoded 0)\n");

    r = pu_stat("/etc", &st);
    if (r == 0 && st.st_size > 0) {
        pu_puts("  /etc size="); pu_puti((int)st.st_size); pu_puts("\n");
        pass("directory size is nonzero (real i_size, not synthesized)");
    } else {
        fail("expected directory size > 0, got ", (int)st.st_size);
    }

    /* ----------------------------------------------------------------- 19 */
    pu_puts("\n[19] metadata: inode numbers\n");

    r = pu_stat("/", &st);
    if (r == 0 && st.st_ino == 2)
        pass("root directory is inode 2 (EXT2_ROOT_INODE)");
    else
        fail("expected root inode 2, got ", (int)st.st_ino);

    {
        struct stat a, b;
        pu_stat("/README.TXT", &a);
        pu_stat("/etc/passwd", &b);
        if (a.st_ino != b.st_ino)
            pass("distinct files have distinct inode numbers");
        else
            fail("README.TXT and passwd share inode ", (int)a.st_ino);

        struct stat a2;
        pu_stat("/README.TXT", &a2);
        if (a.st_ino == a2.st_ino)
            pass("stat'ing the same file twice yields the same inode number");
        else
            fail("inode number unstable across stat calls: ", (int)a2.st_ino);
    }

    /* ----------------------------------------------------------------- 20 */
    pu_puts("\n[20] metadata: permissions, uid, gid\n");

    r = pu_stat("/README.TXT", &st);
    if (r == 0 && (st.st_mode & 0777) == 0644 && st.st_uid == 0 && st.st_gid == 0)
        pass("/README.TXT is mode 0644, uid=0, gid=0");
    else
        fail("unexpected README.TXT metadata, mode&0777=", (int)(st.st_mode & 0777));

    r = pu_stat("/perm/private.txt", &st);
    if (r == 0 && (st.st_mode & 0777) == 0600)
        pass("/perm/private.txt is mode 0600");
    else
        fail("expected mode 0600, got ", (int)(st.st_mode & 0777));

    r = pu_stat("/perm/readonly.txt", &st);
    if (r == 0 && (st.st_mode & 0777) == 0444)
        pass("/perm/readonly.txt is mode 0444");
    else
        fail("expected mode 0444, got ", (int)(st.st_mode & 0777));

    r = pu_stat("/perm/noaccess.bin", &st);
    if (r == 0 && (st.st_mode & 0777) == 0000)
        pass("/perm/noaccess.bin is mode 0000");
    else
        fail("expected mode 0000, got ", (int)(st.st_mode & 0777));

    r = pu_stat("/perm/group_test.txt", &st);
    if (r == 0 && (st.st_mode & 0777) == 0640 && st.st_gid == 100)
        pass("/perm/group_test.txt is mode 0640, gid=100");
    else
        fail("unexpected group_test.txt metadata, gid=", (int)st.st_gid);

    /* ----------------------------------------------------------------- 21 */
    pu_puts("\n[21] metadata: timestamps and link count\n");

    r = pu_stat("/README.TXT", &st);
    if (r == 0 && st.st_atime > 0 && st.st_mtime > 0 && st.st_ctime > 0)
        pass("README.TXT has real (nonzero) atime/mtime/ctime");
    else
        fail("expected nonzero timestamps, mtime=", (int)st.st_mtime);

    if (r == 0 && st.st_nlink == 1)
        pass("regular file has link count 1");
    else
        fail("expected nlink=1 for a regular file, got ", (int)st.st_nlink);

    r = pu_stat("/etc", &st);
    if (r == 0 && st.st_nlink >= 2)
        pass("directory has link count >= 2 ('.' plus parent's entry)");
    else
        fail("expected directory nlink>=2, got ", (int)st.st_nlink);

    /* ----------------------------------------------------------------- 22 */
    pu_puts("\n[22] symlinks: lstat reports the link itself; stat/open follow it (POSIX)\n");

    /* Stage 4 replaced Stage 3A's "symlinks are inert" behavior with real
       POSIX semantics: stat() and open() both follow the final symlink,
       and only lstat() reports the link itself. See tests 25-27 for the
       full follow-through/readlink/lstat coverage; this section just
       confirms the non-following view via lstat() still works. */
    r = pu_lstat("/readme.link", &st);
    if (r == 0 && st.st_type == 3)
        pass("lstat('/readme.link') reports type=SYMLINK(3)");
    else
        fail("expected st_type=3, got ", (int)st.st_type);

    if (r == 0 && S_ISLNK(st.st_mode))
        pass("S_ISLNK(st_mode) is true for readme.link");
    else
        fail("expected S_ISLNK, st_mode=", (int)st.st_mode);

    if (r == 0 && (st.st_mode & 0777) == 0777)
        pass("readme.link inode mode is the conventional 0777 for symlinks");
    else
        fail("expected symlink perm bits 0777, got ", (int)(st.st_mode & 0777));

    fd = pu_open("/readme.link", O_RDONLY);
    if (fd >= 3) {
        pass("open('/readme.link') follows the symlink to a readable regular file");
        pu_close(fd);
    } else
        fail("expected open(symlink) to follow through and succeed, got fd=", fd);

    /* ----------------------------------------------------------------- 23 */
    pu_puts("\n[23] access(): F_OK/R_OK/W_OK/X_OK as root\n");

    if (pu_access("/README.TXT", F_OK) == 0)
        pass("access(README.TXT, F_OK) succeeds — file exists");
    else
        fail("expected F_OK success, got ", pu_access("/README.TXT", F_OK));

    r = pu_access("/no/such/file.txt", F_OK);
    if (r == ENOENT)
        pass("access(nonexistent, F_OK) returns ENOENT");
    else
        fail("expected ENOENT, got ", r);

    if (pu_access("/perm/noaccess.bin", R_OK) == 0)
        pass("root: R_OK on mode-0000 file succeeds (root read bypass)");
    else
        fail("expected root R_OK bypass to succeed, got ", pu_access("/perm/noaccess.bin", R_OK));

    if (pu_access("/perm/readonly.txt", W_OK) == 0)
        pass("root: W_OK on mode-0444 file succeeds (root write bypass)");
    else
        fail("expected root W_OK bypass to succeed, got ", pu_access("/perm/readonly.txt", W_OK));

    if (pu_access("/perm/exec.sh", X_OK) == 0)
        pass("root: X_OK on mode-0755 file succeeds");
    else
        fail("expected X_OK success on exec.sh, got ", pu_access("/perm/exec.sh", X_OK));

    r = pu_access("/perm/noaccess.bin", X_OK);
    if (r == EACCES)
        pass("root: X_OK on mode-0000 file is denied — root still needs "
             "*some* execute bit set (traditional Unix root exception)");
    else
        fail("expected EACCES even for root with no x bits, got ", r);

    /* ----------------------------------------------------------------- 24 */
    pu_puts("\n[24] permission engine: owner/group/other, as non-root\n");
    pu_puts("  (using SYS_DEBUG_SETCRED, a regression-suite-only test hook —\n");
    pu_puts("   see include/pureunix/syscall.h. Credentials are restored to\n");
    pu_puts("   uid=0/gid=0 before this program returns.)\n");

    /* uid=1000 matches neither group_test.txt's owner (0) nor its group
       (100) — everything should fall through to "other". */
    pu_debug_setcred(1000, 999);

    r = pu_access("/perm/private.txt", R_OK);
    if (r == EACCES)
        pass("non-root, no match: R_OK on mode-0600 owner-only file denied");
    else
        fail("expected EACCES on private.txt, got ", r);

    r = pu_access("/perm/readonly.txt", R_OK);
    if (r == 0)
        pass("non-root: R_OK on mode-0444 world-readable file succeeds");
    else
        fail("expected R_OK success on readonly.txt, got ", r);

    r = pu_access("/perm/readonly.txt", W_OK);
    if (r == EACCES)
        pass("non-root: W_OK on mode-0444 file is denied (no bypass for non-root)");
    else
        fail("expected EACCES for W_OK on readonly.txt, got ", r);

    r = pu_access("/perm/group_test.txt", R_OK);
    if (r == EACCES)
        pass("non-root, gid mismatch: R_OK on mode-0640 group file falls "
             "through to other bits (0) and is denied");
    else
        fail("expected EACCES on group_test.txt (gid mismatch), got ", r);

    /* Now match the file's group (gid=100) but still not its owner. */
    pu_debug_setcred(1000, 100);

    r = pu_access("/perm/group_test.txt", R_OK);
    if (r == 0)
        pass("non-root, gid match: R_OK on mode-0640 group file succeeds (group r bit)");
    else
        fail("expected R_OK success via group bits, got ", r);

    r = pu_access("/perm/group_test.txt", W_OK);
    if (r == EACCES)
        pass("non-root, gid match: W_OK on mode-0640 group file denied (group has no w bit)");
    else
        fail("expected EACCES — group bits are r-- only, got ", r);

    r = pu_access("/perm/exec.sh", X_OK);
    if (r == 0)
        pass("non-root: X_OK on mode-0755 file still succeeds (other x bit)");
    else
        fail("expected X_OK success via other bits, got ", r);

    r = pu_access("/perm/noaccess.bin", R_OK);
    int r2 = pu_access("/perm/noaccess.bin", W_OK);
    int r3 = pu_access("/perm/noaccess.bin", X_OK);
    if (r == EACCES && r2 == EACCES && r3 == EACCES)
        pass("non-root: mode-0000 file denies R/W/X with no root bypass");
    else
        fail("expected all three denied for noaccess.bin, r+r2+r3=", r + r2 + r3);

    /* Restoring root is not optional: elf_exec() runs this program's code
       in the shell's own task (there is no fork/exec — see kernel/elf.c),
       so leaving credentials changed here would leave the *shell itself*
       running as uid=1000/gid=100 for the rest of the session. */
    pu_debug_setcred(0, 0);
    if (pu_access("/perm/noaccess.bin", R_OK) == 0)
        pass("credentials restored to uid=0/gid=0 before returning (root bypass confirmed)");
    else
        fail("FATAL: failed to restore root credentials, R_OK=", pu_access("/perm/noaccess.bin", R_OK));

    /* ================================================================
     * Stage 4 — symlinks, pathname resolution, and a writable EXT2.
     * Everything below runs as root (confirmed just above).
     * ================================================================ */

    /* ----------------------------------------------------------------- 25 */
    pu_puts("\n[25] readlink(): absolute target, relative target, truncation\n");

    r = pu_readlink("/abslink", buf, sizeof(buf));
    if (r == 11 && xstrncmp(buf, "/README.TXT", 11) == 0)
        pass("readlink('/abslink') returns the raw absolute target '/README.TXT'");
    else
        fail("expected 11-byte '/README.TXT', got n=", r);

    r = pu_readlink("/readme.link", buf, sizeof(buf));
    if (r == 10 && xstrncmp(buf, "README.TXT", 10) == 0)
        pass("readlink('/readme.link') returns the raw relative target 'README.TXT'");
    else
        fail("expected 10-byte 'README.TXT', got n=", r);

    r = pu_readlink("/testdir/uplink", buf, sizeof(buf));
    if (r == 13 && xstrncmp(buf, "../README.TXT", 13) == 0)
        pass("readlink('/testdir/uplink') returns raw '../README.TXT'");
    else
        fail("expected 13-byte '../README.TXT', got n=", r);

    {
        char small[4];
        r = pu_readlink("/readme.link", small, sizeof(small));
        if (r == 4 && xmemcmp(small, "READ", 4) == 0)
            pass("readlink() truncates to the caller's buffer size (4 bytes of 'README.TXT')");
        else
            fail("expected 4-byte truncated 'READ', got n=", r);
    }

    r = pu_readlink("/README.TXT", buf, sizeof(buf));
    if (r == EINVAL)
        pass("readlink() on a non-symlink returns EINVAL");
    else
        fail("expected EINVAL for readlink on a regular file, got ", r);

    r = pu_readlink("/no/such/link", buf, sizeof(buf));
    if (r == ENOENT)
        pass("readlink() on a nonexistent path returns ENOENT");
    else
        fail("expected ENOENT, got ", r);

    /* ----------------------------------------------------------------- 26 */
    pu_puts("\n[26] symlinks are followed transparently by open/read and stat\n");

    fd = pu_open("/readme.link", O_RDONLY);
    if (fd < 3) {
        fail("expected open('/readme.link') to follow through to README.TXT, fd=", fd);
    } else {
        n = pu_read(fd, buf, 12);
        if (n == 12 && xstrncmp(buf, "PureUnix EXT", 12) == 0)
            pass("reading through readme.link yields README.TXT's real content");
        else
            fail("content read through symlink mismatch, n=", n);
        pu_close(fd);
    }

    r = pu_stat("/readme.link", &st);
    if (r == 0 && st.st_type == 1)
        pass("stat('/readme.link') follows the link and reports type=FILE(1)");
    else
        fail("expected stat to follow to a FILE, got type=", (int)st.st_type);

    r = pu_stat("/abslink", &st);
    if (r == 0 && st.st_type == 1 && st.st_size > 0)
        pass("stat('/abslink') follows the absolute-target link to README.TXT");
    else
        fail("expected abslink to resolve to a nonempty FILE, got type=", (int)st.st_type);

    /* ----------------------------------------------------------------- 27 */
    pu_puts("\n[27] lstat() never follows the final component\n");

    r = pu_lstat("/readme.link", &st);
    if (r == 0 && st.st_type == 3 && S_ISLNK(st.st_mode))
        pass("lstat('/readme.link') reports the symlink itself, type=SYMLINK(3)");
    else
        fail("expected lstat to report SYMLINK, got type=", (int)st.st_type);

    if (r == 0 && (st.st_mode & 0777) == 0777)
        pass("lstat sees the symlink's own conventional 0777 permission bits");
    else
        fail("expected 0777 on the symlink inode, got ", (int)(st.st_mode & 0777));

    r = pu_lstat("/README.TXT", &st);
    if (r == 0 && st.st_type == 1)
        pass("lstat on a non-symlink behaves exactly like stat");
    else
        fail("expected lstat(README.TXT) type=FILE, got ", (int)st.st_type);

    /* ----------------------------------------------------------------- 28 */
    pu_puts("\n[28] symlink loop detection (A -> B -> A) hits ELOOP\n");

    r = pu_stat("/loop_a", &st);
    if (r == ELOOP)
        pass("stat('/loop_a') gives up with ELOOP instead of recursing forever");
    else
        fail("expected ELOOP from stat on a symlink loop, got ", r);

    r = pu_open("/loop_b", O_RDONLY);
    if (r == ELOOP)
        pass("open('/loop_b') also reports ELOOP");
    else
        fail("expected ELOOP from open on a symlink loop, got ", r);

    r = pu_access("/loop_a", F_OK);
    if (r == ELOOP)
        pass("access('/loop_a', F_OK) reports ELOOP rather than ENOENT");
    else
        fail("expected ELOOP from access on a symlink loop, got ", r);

    /* ----------------------------------------------------------------- 29 */
    pu_puts("\n[29] writable EXT2: create, write, close, reopen, read, verify\n");

    fd = pu_creat("/newfile.txt");
    if (fd < 3) {
        fail("FATAL: pu_creat('/newfile.txt') failed, fd=", fd);
    } else {
        const char *msg = "Hello from a writable EXT2 filesystem!\n";
        int msg_len = (int)pu_strlen(msg);
        n = pu_write(fd, msg, msg_len);
        if (n == msg_len)
            pass("write() wrote the full buffer to the new file");
        else
            fail("short write, expected full message, got n=", n);
        pu_close(fd);

        r = pu_stat("/newfile.txt", &st);
        if (r == 0 && (int)st.st_size == msg_len)
            pass("stat() after close() reports the correct new file size");
        else
            fail("expected size=", msg_len);

        fd = pu_open("/newfile.txt", O_RDONLY);
        if (fd < 3) {
            fail("cannot reopen /newfile.txt for reading, fd=", fd);
        } else {
            n = pu_read(fd, buf, sizeof(buf) - 1);
            if (n == msg_len && xstrncmp(buf, msg, msg_len) == 0)
                pass("reopened file's content matches exactly what was written");
            else
                fail("content mismatch after reopen, n=", n);
            pu_close(fd);
        }
    }

    /* ----------------------------------------------------------------- 30 */
    pu_puts("\n[30] mkdir(): '.'/'..' entries, parent link count, traversal\n");

    r = pu_stat("/", &st);
    uint32_t root_nlink_before = st.st_nlink;

    r = pu_mkdir("/newdir");
    if (r == 0)
        pass("mkdir('/newdir') succeeds");
    else
        fail("mkdir('/newdir') failed, r=", r);

    r = pu_stat("/", &st);
    if (r == 0 && st.st_nlink == root_nlink_before + 1)
        pass("parent (/) link count increased by one for the new subdirectory's '..'");
    else
        fail("expected root nlink to increase by 1, got ", (int)st.st_nlink);

    {
        struct dirent entries[8];
        int count = pu_readdir("/newdir", entries, 8);
        if (count == 2 && find_name(entries, count, ".") >= 0 && find_name(entries, count, "..") >= 0)
            pass("new directory contains exactly '.' and '..'");
        else
            fail("expected exactly 2 entries in new dir, got ", count);
    }

    fd = pu_creat("/newdir/inside.txt");
    if (fd >= 3) {
        pu_write(fd, "nested", 6);
        pu_close(fd);
        fd = pu_open("/newdir/inside.txt", O_RDONLY);
        n = fd >= 3 ? pu_read(fd, buf, 6) : -1;
        if (fd >= 3 && n == 6 && xmemcmp(buf, "nested", 6) == 0)
            pass("traversal into a freshly-created directory reaches a freshly-created file");
        else
            fail("traversal into new directory failed, n=", n);
        if (fd >= 3) pu_close(fd);
    } else {
        fail("cannot create file inside new directory, fd=", fd);
    }

    /* ----------------------------------------------------------------- 31 */
    pu_puts("\n[31] unlink(): directory entry removed, inode disappears\n");

    fd = pu_creat("/unlink_me.txt");
    if (fd >= 3) pu_close(fd);

    r = pu_unlink("/unlink_me.txt");
    if (r == 0)
        pass("unlink('/unlink_me.txt') succeeds");
    else
        fail("unlink failed, r=", r);

    r = pu_stat("/unlink_me.txt", &st);
    if (r == ENOENT)
        pass("unlinked file no longer stats successfully");
    else
        fail("expected ENOENT after unlink, got ", r);

    {
        struct dirent entries[64];
        int count = pu_readdir("/", entries, 64);
        if (count >= 0 && find_name(entries, count, "unlink_me.txt") < 0)
            pass("unlinked file no longer appears in its parent's directory listing");
        else
            fail("unlinked file still present in readdir, count=", count);
    }

    /* ----------------------------------------------------------------- 32 */
    pu_puts("\n[32] hard links: same inode, shared data, correct link count\n");

    fd = pu_creat("/hardlink_a.txt");
    if (fd >= 3) {
        pu_write(fd, "original-content", 16);
        pu_close(fd);
    }

    r = pu_link("/hardlink_a.txt", "/hardlink_b.txt");
    if (r == 0)
        pass("link() creates a second name for the same file");
    else
        fail("link() failed, r=", r);

    struct stat sa, sb;
    pu_stat("/hardlink_a.txt", &sa);
    pu_stat("/hardlink_b.txt", &sb);
    if (sa.st_ino == sb.st_ino)
        pass("hard-linked names share the same inode number");
    else
        fail("hard link has a different inode number: ", (int)sb.st_ino);

    if (sa.st_nlink == 2 && sb.st_nlink == 2)
        pass("both names report link count 2");
    else
        fail("expected nlink=2 on both names, got sa=", (int)sa.st_nlink);

    /* Write through B, verify A observes the same new content (proves
       shared data, not a copy). */
    fd = pu_open("/hardlink_b.txt", O_WRONLY | O_TRUNC);
    if (fd >= 3) {
        pu_write(fd, "via-b", 5);
        pu_close(fd);
    }
    fd = pu_open("/hardlink_a.txt", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, buf, 5) : -1;
    if (fd >= 3 && n == 5 && xmemcmp(buf, "via-b", 5) == 0)
        pass("write through the second hard-linked name is visible via the first");
    else
        fail("hard links do not share data as expected, n=", n);
    if (fd >= 3) pu_close(fd);

    r = pu_unlink("/hardlink_a.txt");
    pu_stat("/hardlink_b.txt", &sb);
    if (r == 0 && sb.st_nlink == 1)
        pass("unlinking one hard-linked name drops the link count to 1, file still accessible");
    else
        fail("expected nlink=1 after unlinking one of two links, got ", (int)sb.st_nlink);

    fd = pu_open("/hardlink_b.txt", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, buf, 5) : -1;
    if (fd >= 3 && n == 5 && xmemcmp(buf, "via-b", 5) == 0)
        pass("remaining hard-linked name still reads the shared content correctly");
    else
        fail("remaining hard link unreadable/corrupted, n=", n);
    if (fd >= 3) pu_close(fd);

    r = pu_unlink("/hardlink_b.txt");
    if (r == 0 && pu_stat("/hardlink_b.txt", &sb) == ENOENT)
        pass("unlinking the last hard link frees the inode (ENOENT afterward)");
    else
        fail("expected the inode to disappear once nlink reached 0", 0);

    r = pu_link("/hardlink_a.txt", "/should_fail.txt");
    if (r == ENOENT)
        pass("link() on an already-fully-unlinked source correctly fails with ENOENT");
    else
        fail("expected ENOENT linking a gone file, got ", r);

    r = pu_link("/", "/root_link_should_fail");
    if (r == EPERM)
        pass("link() refuses to hard-link a directory (EPERM)");
    else
        fail("expected EPERM linking a directory, got ", r);

    /* ----------------------------------------------------------------- 33 */
    pu_puts("\n[33] rename(): within a directory, across directories, over a target, EXDEV\n");

    fd = pu_creat("/rename_src.txt");
    if (fd >= 3) { pu_write(fd, "rename-me", 9); pu_close(fd); }

    r = pu_rename("/rename_src.txt", "/rename_dst.txt");
    if (r == 0 && pu_stat("/rename_src.txt", &st) == ENOENT && pu_stat("/rename_dst.txt", &st) == 0)
        pass("rename() within the same directory moves the name, old name gone");
    else
        fail("same-directory rename failed, r=", r);

    r = pu_rename("/rename_dst.txt", "/testdir/rename_moved.txt");
    if (r == 0 && pu_stat("/rename_dst.txt", &st) == ENOENT) {
        fd = pu_open("/testdir/rename_moved.txt", O_RDONLY);
        n = fd >= 3 ? pu_read(fd, buf, 9) : -1;
        if (fd >= 3 && n == 9 && xmemcmp(buf, "rename-me", 9) == 0)
            pass("cross-directory rename moved both the name and the content");
        else
            fail("moved file unreadable/corrupted after cross-directory rename, n=", n);
        if (fd >= 3) pu_close(fd);
    } else {
        fail("cross-directory rename failed, r=", r);
    }

    fd = pu_creat("/rename_over_target.txt");
    if (fd >= 3) { pu_write(fd, "target-original", 15); pu_close(fd); }
    pu_stat("/testdir/rename_moved.txt", &sa);

    r = pu_rename("/testdir/rename_moved.txt", "/rename_over_target.txt");
    if (r == 0) {
        pu_stat("/rename_over_target.txt", &sb);
        fd = pu_open("/rename_over_target.txt", O_RDONLY);
        n = fd >= 3 ? pu_read(fd, buf, 9) : -1;
        if (sa.st_ino == sb.st_ino && n == 9 && xmemcmp(buf, "rename-me", 9) == 0)
            pass("rename() over an existing file replaces it, inode carried over from the source");
        else
            fail("rename-over-existing did not replace correctly, n=", n);
        if (fd >= 3) pu_close(fd);
    } else {
        fail("rename over an existing destination failed, r=", r);
    }

    fd = pu_creat("/cross_device.txt");
    if (fd >= 3) pu_close(fd);
    r = pu_rename("/cross_device.txt", "/fat/cross_device.txt");
    if (r == EXDEV)
        pass("rename() across filesystems (EXT2 -> FAT16 /fat) correctly fails with EXDEV");
    else
        fail("expected EXDEV for a cross-filesystem rename, got ", r);
    pu_unlink("/cross_device.txt");

    /* ----------------------------------------------------------------- 34 */
    pu_puts("\n[34] rmdir(): rejects non-empty, accepts empty\n");

    r = pu_mkdir("/rmdir_test");
    fd = pu_creat("/rmdir_test/occupant.txt");
    if (fd >= 3) pu_close(fd);

    r = pu_rmdir("/rmdir_test");
    if (r == ENOTEMPTY)
        pass("rmdir() on a non-empty directory fails with ENOTEMPTY");
    else
        fail("expected ENOTEMPTY for a non-empty directory, got ", r);

    pu_unlink("/rmdir_test/occupant.txt");
    r = pu_rmdir("/rmdir_test");
    if (r == 0 && pu_stat("/rmdir_test", &st) == ENOENT)
        pass("rmdir() on an now-empty directory succeeds and it disappears");
    else
        fail("rmdir of an empty directory failed, r=", r);

    /* ----------------------------------------------------------------- 35 */
    pu_puts("\n[35] symlink(): absolute, relative, fast vs block-based\n");

    r = pu_symlink("/README.TXT", "/newlink_abs");
    if (r == 0) {
        n = pu_readlink("/newlink_abs", buf, sizeof(buf));
        if (n == 11 && xstrncmp(buf, "/README.TXT", 11) == 0)
            pass("symlink() creates an absolute-target link readlink() reads back correctly");
        else
            fail("newlink_abs readlink mismatch, n=", n);

        r = pu_stat("/newlink_abs", &st);
        if (r == 0 && st.st_type == 1)
            pass("the new absolute symlink is followed correctly by stat()");
        else
            fail("expected stat to follow newlink_abs to a FILE, got ", (int)st.st_type);
    } else {
        fail("symlink('/README.TXT', '/newlink_abs') failed, r=", r);
    }

    r = pu_symlink("../README.TXT", "/testdir/newlink_rel");
    if (r == 0) {
        fd = pu_open("/testdir/newlink_rel", O_RDONLY);
        n = fd >= 3 ? pu_read(fd, buf, 12) : -1;
        if (fd >= 3 && n == 12 && xstrncmp(buf, "PureUnix EXT", 12) == 0)
            pass("relative symlink resolves from its own parent directory, not cwd");
        else
            fail("relative symlink did not resolve correctly, n=", n);
        if (fd >= 3) pu_close(fd);
    } else {
        fail("symlink('../README.TXT', '/testdir/newlink_rel') failed, r=", r);
    }

    r = pu_lstat("/newlink_abs", &st);
    if (r == 0 && st.st_blocks == 0)
        pass("a short (fast) symlink target allocates zero data blocks");
    else
        fail("expected a fast symlink to have st_blocks=0, got ", (int)st.st_blocks);

    {
        /* Every "./" is a resolver no-op (see fs/vfs.c's resolve_path), so
           this target is 80 raw bytes but still ultimately resolves to
           /README.TXT — long enough to force real data-block allocation
           instead of the 60-byte inline fast-symlink path. */
        const char *long_target =
            "./././././././././././././././././././././././././././././././././././././"
            "README.TXT";
        r = pu_symlink(long_target, "/longlink");
        if (r == 0) {
            n = pu_readlink("/longlink", buf, sizeof(buf));
            int tlen = (int)pu_strlen(long_target);
            if (n == tlen && xstrncmp(buf, long_target, tlen) == 0)
                pass("long (block-based) symlink target reads back byte-for-byte");
            else
                fail("long symlink readlink mismatch, n=", n);

            r = pu_lstat("/longlink", &st);
            if (r == 0 && st.st_blocks > 0)
                pass("a long symlink target allocates at least one real data block");
            else
                fail("expected st_blocks>0 for a long symlink, got ", (int)st.st_blocks);

            fd = pu_open("/longlink", O_RDONLY);
            n = fd >= 3 ? pu_read(fd, buf, 12) : -1;
            if (fd >= 3 && n == 12 && xstrncmp(buf, "PureUnix EXT", 12) == 0)
                pass("the long symlink still resolves through all its './' components to README.TXT");
            else
                fail("long symlink did not resolve to README.TXT, n=", n);
            if (fd >= 3) pu_close(fd);
        } else {
            fail("symlink() with a >60-byte target failed, r=", r);
        }
    }

    /* ----------------------------------------------------------------- 36 */
    pu_puts("\n[36] stress test: hundreds of creates/deletes, allocator stays consistent\n");

    {
        enum { STRESS_COUNT = 200 };
        r = pu_mkdir("/stress");
        if (r != 0) {
            fail("FATAL: mkdir('/stress') failed, r=", r);
        } else {
            int created = 0;
            for (int i = 0; i < STRESS_COUNT; i++) {
                char name[32];
                int p = 0;
                const char *prefix = "/stress/f";
                for (int k = 0; prefix[k]; k++) name[p++] = prefix[k];
                if (i >= 100) name[p++] = (char)('0' + (i / 100) % 10);
                if (i >= 10) name[p++] = (char)('0' + (i / 10) % 10);
                name[p++] = (char)('0' + i % 10);
                name[p++] = '\0';

                fd = pu_creat(name);
                if (fd >= 3) {
                    pu_write(fd, "x", 1);
                    pu_close(fd);
                    created++;
                }
            }
            if (created == STRESS_COUNT)
                pass("created 200 files in a fresh directory");
            else
                fail("expected to create 200 files, only succeeded for ", created);

            struct dirent entries[256];
            int count = pu_readdir("/stress", entries, 256);
            if (count == STRESS_COUNT + 2) /* plus '.' and '..' */
                pass("directory listing shows exactly 200 files plus '.' and '..'");
            else
                fail("expected 202 directory entries, got ", count);

            /* Delete every other file, freeing roughly half the inodes and
               blocks this directory's contents used. */
            int deleted = 0;
            for (int i = 0; i < STRESS_COUNT; i += 2) {
                char name[32];
                int p = 0;
                const char *prefix = "/stress/f";
                for (int k = 0; prefix[k]; k++) name[p++] = prefix[k];
                if (i >= 100) name[p++] = (char)('0' + (i / 100) % 10);
                if (i >= 10) name[p++] = (char)('0' + (i / 10) % 10);
                name[p++] = (char)('0' + i % 10);
                name[p++] = '\0';
                if (pu_unlink(name) == 0) deleted++;
            }
            if (deleted == STRESS_COUNT / 2)
                pass("deleted every other file (100 of 200) successfully");
            else
                fail("expected 100 deletions, got ", deleted);

            /* Recreate them, reusing the freed inodes/blocks; if the
               allocator double-books anything this will corrupt data or
               fail outright. */
            int recreated = 0;
            for (int i = 0; i < STRESS_COUNT; i += 2) {
                char name[32];
                int p = 0;
                const char *prefix = "/stress/f";
                for (int k = 0; prefix[k]; k++) name[p++] = prefix[k];
                if (i >= 100) name[p++] = (char)('0' + (i / 100) % 10);
                if (i >= 10) name[p++] = (char)('0' + (i / 10) % 10);
                name[p++] = (char)('0' + i % 10);
                name[p++] = '\0';

                fd = pu_creat(name);
                if (fd >= 3) {
                    pu_write(fd, "y", 1);
                    pu_close(fd);
                    recreated++;
                }
            }
            if (recreated == STRESS_COUNT / 2)
                pass("reused freed inodes/blocks to recreate 100 files");
            else
                fail("expected 100 recreations to succeed, got ", recreated);

            count = pu_readdir("/stress", entries, 256);
            if (count == STRESS_COUNT + 2)
                pass("directory entry count is back to 202 after delete+recreate");
            else
                fail("expected 202 entries after recreate cycle, got ", count);

            /* Verify a sample of both never-deleted and recreated files
               still (or again) read back their expected content. */
            fd = pu_open("/stress/f1", O_RDONLY); /* never deleted (odd index) */
            n = fd >= 3 ? pu_read(fd, buf, 1) : -1;
            int sample1_ok = (fd >= 3 && n == 1 && buf[0] == 'x');
            if (fd >= 3) pu_close(fd);

            fd = pu_open("/stress/f0", O_RDONLY); /* deleted then recreated */
            n = fd >= 3 ? pu_read(fd, buf, 1) : -1;
            int sample2_ok = (fd >= 3 && n == 1 && buf[0] == 'y');
            if (fd >= 3) pu_close(fd);

            if (sample1_ok && sample2_ok)
                pass("both untouched and recreated files read back correct, uncorrupted content");
            else
                fail("stress-test content verification failed", 0);

            /* Tear everything down and confirm rmdir succeeds once empty. */
            for (int i = 0; i < STRESS_COUNT; i++) {
                char name[32];
                int p = 0;
                const char *prefix = "/stress/f";
                for (int k = 0; prefix[k]; k++) name[p++] = prefix[k];
                if (i >= 100) name[p++] = (char)('0' + (i / 100) % 10);
                if (i >= 10) name[p++] = (char)('0' + (i / 10) % 10);
                name[p++] = (char)('0' + i % 10);
                name[p++] = '\0';
                pu_unlink(name);
            }
            r = pu_rmdir("/stress");
            if (r == 0)
                pass("after deleting all 200 files, rmdir('/stress') succeeds");
            else
                fail("expected rmdir to succeed once /stress was emptied, r=", r);
        }
    }

    pu_puts("\n=== ext2test complete ===\n");
    return 0;
}
