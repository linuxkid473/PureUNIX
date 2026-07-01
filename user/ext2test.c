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
    pu_puts("\n[22] symlinks: detected, not followed, not readable as a file\n");

    r = pu_stat("/readme.link", &st);
    if (r == 0 && st.st_type == 3)
        pass("stat('/readme.link') reports type=SYMLINK(3)");
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
    if (fd < 0)
        pass("open('/readme.link') fails — readlink() is not implemented, "
             "and a symlink must not be readable as a regular file");
    else {
        fail("expected open(symlink) to fail, got fd=", fd);
        pu_close(fd);
    }

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

    pu_puts("\n=== ext2test complete ===\n");
    return 0;
}
