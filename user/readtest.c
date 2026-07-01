#include "libpure.h"

/* ------------------------------------------------------------------ helpers */

static int local_memcmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i])
            return 1;
    }
    return 0;
}

/* Print n bytes, substituting '.' for non-printable chars. */
static void print_bytes(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c >= 32 && c < 127)
            pu_write(1, &c, 1);
        else
            pu_puts(".");
    }
}

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

/* ------------------------------------------------------------------ main */

int main(void)
{
    pu_puts("=== readtest ===\n");

    char buf1[32], buf2[32];
    int n, r, fd;

    /* ---- 1. Basic sequential reads --------------------------------- */
    pu_puts("\n[1] Basic sequential reads\n");

    fd = pu_open("/README.TXT", O_RDONLY);
    if (fd < 3) {
        pu_puts("FATAL: cannot open /README.TXT\n");
        return 1;
    }

    n = pu_read(fd, buf1, 32);
    pu_puts("  first 32 bytes (n="); pu_puti(n); pu_puts("): [");
    print_bytes(buf1, n > 0 ? n : 0);
    pu_puts("]\n");
    if (n == 32) pass("read(fd,buf,32) returns 32");
    else         fail("expected 32, got ", n);

    n = pu_read(fd, buf2, 32);
    pu_puts("  next  32 bytes (n="); pu_puti(n); pu_puts("): [");
    print_bytes(buf2, n > 0 ? n : 0);
    pu_puts("]\n");
    if (n == 32) pass("second read(fd,buf,32) returns 32");
    else         fail("expected 32, got ", n);

    /* ---- 2. Seek back and verify re-read matches ------------------- */
    pu_puts("\n[2] SEEK_SET 0 and re-read\n");

    r = pu_lseek(fd, 0, SEEK_SET);
    if (r != 0) fail("lseek(SEEK_SET,0) returned ", r);

    char verify[32];
    n = pu_read(fd, verify, 32);
    if (n == 32 && local_memcmp(buf1, verify, 32) == 0)
        pass("re-read after SEEK_SET 0 matches original first read");
    else
        fail("re-read mismatch, n=", n);

    /* ---- 3. Read at EOF returns 0 ---------------------------------- */
    pu_puts("\n[3] Read at EOF\n");

    pu_lseek(fd, 0, SEEK_END);
    n = pu_read(fd, buf1, 32);
    if (n == 0) pass("read at EOF returns 0");
    else        fail("expected 0 at EOF, got ", n);

    /* Idempotent: second read at EOF still returns 0 */
    n = pu_read(fd, buf1, 32);
    if (n == 0) pass("second read at EOF still returns 0");
    else        fail("expected 0, got ", n);

    /* ---- 4. Read exactly to EOF ------------------------------------ */
    pu_puts("\n[4] Read exactly to EOF\n");

    struct stat st;
    pu_stat("/README.TXT", &st);
    int fsize = (int)st.st_size;
    pu_puts("  file size = "); pu_puti(fsize); pu_puts("\n");

    if (fsize >= 8) {
        char tmp[16];
        pu_lseek(fd, fsize - 8, SEEK_SET);
        n = pu_read(fd, tmp, 8);
        if (n == 8) pass("read 8 bytes ending exactly at EOF");
        else        fail("expected 8, got ", n);

        /* Next read must see EOF */
        n = pu_read(fd, tmp, 8);
        if (n == 0) pass("read immediately after exact-EOF returns 0");
        else        fail("expected 0, got ", n);
    }

    /* ---- 5. Zero-length read --------------------------------------- */
    pu_puts("\n[5] Zero-length read\n");

    pu_lseek(fd, 0, SEEK_SET);
    n = pu_read(fd, buf1, 0);
    if (n == 0) pass("read(fd,buf,0) returns 0");
    else        fail("expected 0, got ", n);

    /* Offset must be unchanged: next 4-byte read starts at 0 */
    n = pu_read(fd, buf1, 4);
    if (n == 4) pass("offset unchanged after zero-length read");
    else        fail("expected 4, got ", n);

    /* ---- 6. Repeated 1-byte reads ---------------------------------- */
    pu_puts("\n[6] Repeated 1-byte reads\n");

    pu_lseek(fd, 0, SEEK_SET);
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        char c;
        if (pu_read(fd, &c, 1) != 1) { ok = 0; break; }
    }
    if (ok) pass("16 consecutive 1-byte reads all returned 1");
    else    pu_puts("  [FAIL] 1-byte read loop failed\n");

    /* ---- 7. SEEK_CUR as ftell, then relative seek back ------------- */
    pu_puts("\n[7] SEEK_CUR: get current position and seek relative\n");

    pu_lseek(fd, 0, SEEK_SET);
    pu_read(fd, buf1, 8);

    int cur = pu_lseek(fd, 0, SEEK_CUR);   /* standard ftell idiom */
    if (cur == 8) pass("SEEK_CUR+0 after 8-byte read returns 8");
    else          fail("expected 8, got ", cur);

    r = pu_lseek(fd, -4, SEEK_CUR);         /* step back 4 bytes */
    if (r == 4) pass("SEEK_CUR -4 from pos 8 returns 4");
    else        fail("expected 4, got ", r);

    /* ---- 8. NULL buffer -------------------------------------------- */
    pu_puts("\n[8] NULL buffer\n");

    n = pu_read(fd, (char *)0, 32);
    if (n == EINVAL) pass("read(fd,NULL,32) returns EINVAL");
    else             fail("expected EINVAL(-22), got ", n);

    /* ---- 9. Read after close --------------------------------------- */
    pu_puts("\n[9] Read after close\n");

    pu_close(fd);
    n = pu_read(fd, buf1, 32);
    if (n == EBADF) pass("read after close returns EBADF");
    else            fail("expected EBADF(-9), got ", n);

    /* ---- 10. Invalid fd values ------------------------------------- */
    pu_puts("\n[10] Invalid fd values\n");

    n = pu_read(-1, buf1, 32);
    if (n == EBADF) pass("read(-1,...) returns EBADF");
    else            fail("expected EBADF(-9), got ", n);

    n = pu_read(99, buf1, 32);
    if (n == EBADF) pass("read(99,...) returns EBADF");
    else            fail("expected EBADF(-9), got ", n);

    /* stdout and stderr are write-only */
    n = pu_read(1, buf1, 32);
    if (n == EBADF) pass("read(stdout=1,...) returns EBADF");
    else            fail("expected EBADF(-9), got ", n);

    n = pu_read(2, buf1, 32);
    if (n == EBADF) pass("read(stderr=2,...) returns EBADF");
    else            fail("expected EBADF(-9), got ", n);

    /* ---- 11. Multiple simultaneously open file descriptors --------- */
    pu_puts("\n[11] Multiple simultaneously open fds\n");

    int fa = pu_open("/README.TXT", O_RDONLY);
    int fb = pu_open("/README.TXT", O_RDONLY);

    if (fa >= 3 && fb >= 3) {
        pu_puts("  fa="); pu_puti(fa);
        pu_puts("  fb="); pu_puti(fb); pu_puts("\n");

        char a[8], b[8];
        int na = pu_read(fa, a, 8);
        int nb = pu_read(fb, b, 8);

        if (na == 8 && nb == 8 && local_memcmp(a, b, 8) == 0)
            pass("two fds on same file read identical first 8 bytes");
        else
            fail("two-fd read mismatch, na=", na);

        /* Seeking fa must not perturb fb's offset */
        pu_lseek(fa, 4, SEEK_SET);
        int fb_pos = pu_lseek(fb, 0, SEEK_CUR);
        if (fb_pos == 8)
            pass("seeking fa does not affect fb (independent offsets)");
        else
            fail("fb offset wrong, expected 8, got ", fb_pos);

        /* Read from fa at its new position (4) */
        char fa_buf[4];
        na = pu_read(fa, fa_buf, 4);
        if (na == 4) pass("read from fa at seeked position returns 4");
        else         fail("expected 4, got ", na);

        pu_close(fa);
        pu_close(fb);
    } else {
        pu_puts("  (skip: could not open two fds simultaneously)\n");
        if (fa >= 3) pu_close(fa);
        if (fb >= 3) pu_close(fb);
    }

    pu_puts("\n=== readtest complete ===\n");
    return 0;
}
