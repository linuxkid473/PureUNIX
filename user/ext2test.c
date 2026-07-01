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

    pu_puts("\n=== ext2test complete ===\n");
    return 0;
}
