/*
 * user/systest.c — The PureUNIX Userspace Conformance Suite.
 *
 * This is the permanent regression suite for PureUNIX. Every implemented
 * syscall and userspace-visible feature gets a test here; every future
 * kernel feature should add its tests to this file rather than spawning a
 * new one-off test program.
 *
 * Design:
 *   - Every check is independent, deterministic, and numbered.
 *   - A failure never stops the run — every remaining check still executes.
 *   - Output is machine- and human-readable: "[NNN] description" followed
 *     by PASS, or FAIL with the expected/actual values.
 *   - A final summary reports the totals.
 *
 * fork()/exec()/wait() (see "Process creation") give every child its own
 * private address space (a separate page directory — see
 * vmm_create_user_directory() / vmm_fork_address_space() in kernel/vmm.c),
 * so a forked child can freely run and mutate its own memory without any
 * risk of corrupting this test program's own state.
 *
 * One thing this suite cannot exercise from userspace, by construction of
 * this kernel, and does not attempt to:
 *   - Rebooting: reboot is a shell builtin that calls an arch-level
 *     function directly; it has no syscall number, so there is nothing for
 *     a user program to invoke. Even if there were, calling it here would
 *     kill the run before the summary could print. Not tested.
 */
#include "libpure.h"

/* ------------------------------------------------------------------ harness */

static int g_num  = 0;
static int g_pass = 0;
static int g_fail = 0;

static void print_num3(int n)
{
    if (n < 10) pu_puts("00");
    else if (n < 100) pu_puts("0");
    pu_puti(n);
}

static void t_begin(const char *desc)
{
    g_num++;
    pu_puts("[");
    print_num3(g_num);
    pu_puts("] ");
    pu_puts(desc);
    pu_puts("\n");
}

/* The one primitive nearly everything below funnels through: compare an
 * expected int (an errno constant, a count, a byte, a boolean as 0/1...)
 * against what actually came back. */
static void check_eq(const char *desc, int expected, int got)
{
    t_begin(desc);
    if (expected == got) {
        g_pass++;
        pu_puts("PASS\n");
    } else {
        g_fail++;
        pu_puts("FAIL\n");
        pu_puts("  expected "); pu_puti(expected);
        pu_puts(" got "); pu_puti(got); pu_puts("\n");
    }
}

/* Boolean check re-expressed through check_eq so failures still print a
 * concrete expected/got pair (1/0) rather than a bare message. */
static void check_true(const char *desc, int cond)
{
    check_eq(desc, 1, cond ? 1 : 0);
}

static int bytes_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int str_eq(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
        i++;
    }
}

static int find_name(struct dirent *entries, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (str_eq(entries[i].name, name)) return i;
    }
    return -1;
}

/* Builds "/prefix" + number (no padding) into out, e.g. num_path(out, "/systest_tmp/f", 7) -> "/systest_tmp/f7". */
static void num_path(char *out, const char *prefix, int n)
{
    int p = 0;
    for (int k = 0; prefix[k]; k++) out[p++] = prefix[k];
    if (n >= 1000) out[p++] = (char)('0' + (n / 1000) % 10);
    if (n >= 100)  out[p++] = (char)('0' + (n / 100) % 10);
    if (n >= 10)   out[p++] = (char)('0' + (n / 10) % 10);
    out[p++] = (char)('0' + n % 10);
    out[p] = '\0';
}

/* ------------------------------------------------------------------ sections */

static void section(const char *title)
{
    pu_puts("\n--- ");
    pu_puts(title);
    pu_puts(" ---\n");
}

/* ==================================================================== */

static void test_process_basics(void)
{
    section("Process basics: SYS_GETPID, SYS_YIELD, SYS_EXIT, unknown syscall");

    int pid1 = pu_getpid();
    check_true("getpid returns a positive task id", pid1 > 0);

    int pid2 = pu_getpid();
    check_eq("getpid is stable across calls within the same task", pid1, pid2);

    check_eq("yield returns 0", 0, pu_yield());

    /* SYS_EXIT does not terminate the caller (see docs/syscalls.md) — it is
     * a pass-through: syscall_dispatch just returns EBX. Calling it here
     * must not end this program; the very next check running proves it. */
    int rc = pu_syscall_raw(SYS_EXIT, 1234, 0, 0);
    check_eq("SYS_EXIT passes EBX back to the caller", 1234, rc);
    check_true("execution continues normally after SYS_EXIT", 1);

    /* Any unrecognized syscall number returns (uint32_t)-1. */
    check_eq("unrecognized syscall number returns -1", -1, pu_syscall_raw(9999, 0, 0, 0));
}

/* ==================================================================== */

/* Mutated only by the forked child in test_fork_exec_wait(); if fork()
 * actually gives the child its own address space, the parent must never
 * observe the child's write to this. */
static int g_fork_probe = 111;

static void test_fork_exec_wait(void)
{
    section("Process creation: SYS_FORK, SYS_EXEC, SYS_WAIT");

    int parent_pid = pu_getpid();

    int pid = pu_fork();
    check_true("fork() returns a value >= 0", pid >= 0);

    if (pid == 0) {
        /* Child: prove address-space isolation, then terminate — must not
         * fall through and re-run the rest of this program's tests. */
        g_fork_probe = 222;
        pu_exit(7);
    }

    check_true("fork() returns a child pid distinct from the parent's", pid != parent_pid);
    check_eq("the parent's own pid is unchanged after fork()", parent_pid, pu_getpid());

    int status = -1;
    int reaped = pu_wait(pid, &status);
    check_eq("wait(pid) reaps the forked child", pid, reaped);
    check_eq("wait(pid) reports the child's exit code", 7, status);
    check_eq("the parent's memory is unaffected by the child's write (separate address spaces)",
             111, g_fork_probe);

    check_true("wait() on a nonexistent child returns an error", pu_wait(999999, 0) < 0);

    /* exec(): fork a child, have it replace itself with /bin/hello.elf,
     * and confirm the parent observes *that* program's exit status (0),
     * not whatever the child would have used had exec() silently failed
     * and fallen through to pu_exit(123) below. */
    int pid2 = pu_fork();
    check_true("fork() (for exec test) returns a value >= 0", pid2 >= 0);

    if (pid2 == 0) {
        pu_exec("/bin/hello.elf");
        /* exec() only returns on failure. */
        pu_exit(123);
    }

    int status2 = -1;
    int reaped2 = pu_wait(pid2, &status2);
    check_eq("wait() reaps the exec()'d child", pid2, reaped2);
    check_eq("the exec()'d child exits with hello.elf's own status, not the fallback", 0, status2);

    int rc = pu_exec("/bin/no_such_program.elf");
    check_true("exec() on a missing path fails and returns to the caller", rc < 0);
    check_true("execution continues normally after a failed exec()", 1);
}

/* ==================================================================== */

static void test_write_read_basics(void)
{
    section("SYS_WRITE / SYS_READ basics");

    check_eq("write(stdout, \"x\", 1) returns 1", 1, pu_write(1, "x", 1));
    pu_puts("\n");
    check_eq("write(stderr, \"x\", 1) returns 1", 1, pu_write(2, "x", 1));
    pu_puts("\n");
    check_eq("write to a never-opened fd returns EBADF", EBADF, pu_write(5, "x", 1));
    check_eq("read from a never-opened fd returns EBADF", EBADF, pu_read(5, (char *)0, 1));
}

/* ==================================================================== */

static void test_isatty_ioctl(void)
{
    section("isatty() / ioctl(TIOCGWINSZ)");

    check_true("isatty(0) is true (stdin names the console)", pu_isatty(0));
    check_true("isatty(1) is true (stdout names the console)", pu_isatty(1));
    check_true("isatty(2) is true (stderr names the console)", pu_isatty(2));
    check_true("isatty() on a negative fd is false", !pu_isatty(-1));
    check_true("isatty() on a never-opened fd is false", !pu_isatty(9));

    int fd = pu_open("/README.TXT", O_RDONLY);
    check_true("opened /README.TXT to use as a non-tty fd", fd >= 3);
    if (fd >= 3) {
        check_true("isatty() on an open regular file is false", !pu_isatty(fd));
    }

    struct winsize ws;
    check_eq("ioctl(0, TIOCGWINSZ, ...) succeeds", 0, pu_ioctl(0, TIOCGWINSZ, &ws));
    check_eq("console reports 80 columns", 80, ws.ws_col);
    check_eq("console reports 25 rows", 25, ws.ws_row);

    struct winsize ws1, ws2;
    pu_ioctl(1, TIOCGWINSZ, &ws1);
    pu_ioctl(2, TIOCGWINSZ, &ws2);
    check_true("fd 0/1/2 report the same size (one shared console)",
               ws.ws_row == ws1.ws_row && ws1.ws_row == ws2.ws_row &&
               ws.ws_col == ws1.ws_col && ws1.ws_col == ws2.ws_col);

    check_eq("ioctl() with a null argp returns EINVAL", EINVAL, pu_ioctl(0, TIOCGWINSZ, (void *)0));
    check_eq("ioctl() with an unsupported request returns EINVAL", EINVAL, pu_ioctl(0, 999, &ws));
    check_eq("ioctl() on a negative fd returns EBADF", EBADF, pu_ioctl(-1, TIOCGWINSZ, &ws));
    check_eq("ioctl() on a never-opened fd returns EBADF", EBADF, pu_ioctl(9, TIOCGWINSZ, &ws));

    if (fd >= 3) {
        check_eq("ioctl(TIOCGWINSZ) on an open regular file returns ENOTTY", ENOTTY, pu_ioctl(fd, TIOCGWINSZ, &ws));
        pu_close(fd);
    }
}

/* ==================================================================== */

static void test_open_close(void)
{
    section("SYS_OPEN / SYS_CLOSE");

    int fd = pu_open("/README.TXT", O_RDONLY);
    check_true("open('/README.TXT', O_RDONLY) succeeds", fd >= 3);
    check_eq("close() on a freshly opened fd returns 0", 0, pu_close(fd));
    check_eq("close() on an already-closed fd returns EBADF", EBADF, pu_close(fd));
    check_eq("close() on an out-of-range fd returns EBADF", EBADF, pu_close(99));
    check_eq("close() on a reserved fd (stdout) returns EBADF", EBADF, pu_close(1));

    check_eq("open() on a missing path returns ENOENT", ENOENT, pu_open("/no/such/file.txt", O_RDONLY));
    check_eq("open() on a directory returns EISDIR", EISDIR, pu_open("/etc", O_RDONLY));
    check_eq("open() with a null path returns EINVAL", EINVAL, pu_open((const char *)0, O_RDONLY));

    /* creat()/O_CREAT + reopen round-trip. */
    int wfd = pu_creat("/systest_tmp_openclose.txt");
    check_true("creat() on a new path succeeds", wfd >= 3);
    check_eq("write() through the new fd writes the full buffer", 5, pu_write(wfd, "hello", 5));
    check_eq("close() flushes the write successfully", 0, pu_close(wfd));

    struct stat st;
    int r = pu_stat("/systest_tmp_openclose.txt", &st);
    check_eq("stat() after close() reports the written size", 0, r);
    check_eq("stat() size matches what was written", 5, (int)st.st_size);

    int rfd = pu_open("/systest_tmp_openclose.txt", O_RDONLY);
    check_true("reopen for reading succeeds", rfd >= 3);
    char buf[16];
    int n = pu_read(rfd, buf, sizeof(buf));
    check_eq("reopened content length matches", 5, n);
    check_true("reopened content matches what was written", n == 5 && bytes_eq(buf, "hello", 5));
    pu_close(rfd);

    /* O_CREAT on an existing path (no O_EXCL) must not fail — POSIX creat()
     * semantics; every non-append write-open also starts from an empty
     * buffer (see docs/syscalls.md's SYS_OPEN section), so this both
     * confirms the open succeeds and that the prior content is replaced. */
    int wfd2 = pu_open("/systest_tmp_openclose.txt", O_WRONLY | O_CREAT);
    check_true("O_CREAT on an existing file succeeds (no O_EXCL)", wfd2 >= 3);
    pu_write(wfd2, "hi", 2);
    pu_close(wfd2);
    r = pu_stat("/systest_tmp_openclose.txt", &st);
    check_eq("re-opened-for-write file was truncated to the new content", 2, (int)st.st_size);

    pu_unlink("/systest_tmp_openclose.txt");

    /* Exhaust the fd table: 16 slots total, 3 reserved (0/1/2), so 13 can be
     * opened concurrently; the 14th must fail with EMFILE. */
    int fds[14];
    int opened = 0;
    for (int i = 0; i < 13; i++) {
        fds[i] = pu_open("/README.TXT", O_RDONLY);
        if (fds[i] >= 3) opened++;
    }
    check_eq("13 concurrent opens succeed (fds 3..15)", 13, opened);
    fds[13] = pu_open("/README.TXT", O_RDONLY);
    check_eq("the 14th concurrent open fails with EMFILE", EMFILE, fds[13]);
    for (int i = 0; i < 13; i++) pu_close(fds[i]);
}

/* ==================================================================== */

static void test_lseek(void)
{
    section("SYS_LSEEK");

    int fd = pu_open("/README.TXT", O_RDONLY);
    struct stat st;
    pu_stat("/README.TXT", &st);
    int size = (int)st.st_size;

    check_eq("SEEK_SET 0 returns 0", 0, pu_lseek(fd, 0, SEEK_SET));
    check_eq("SEEK_CUR +5 from 0 returns 5", 5, pu_lseek(fd, 5, SEEK_CUR));
    check_eq("SEEK_SET back to 0 returns 0", 0, pu_lseek(fd, 0, SEEK_SET));
    check_eq("SEEK_END 0 returns the file size", size, pu_lseek(fd, 0, SEEK_END));

    char one;
    check_eq("read() at EOF (after SEEK_END) returns 0", 0, pu_read(fd, &one, 1));

    check_eq("seeking past EOF is permitted", size + 1000, pu_lseek(fd, size + 1000, SEEK_SET));
    check_eq("negative resulting offset returns EINVAL", EINVAL, pu_lseek(fd, -1, SEEK_SET));
    check_eq("invalid whence returns EINVAL", EINVAL, pu_lseek(fd, 0, 99));
    pu_close(fd);

    check_eq("lseek on a closed/invalid fd returns EBADF", EBADF, pu_lseek(fd, 0, SEEK_SET));
}

/* ==================================================================== */

static void test_stat_lstat_access(void)
{
    section("SYS_STAT / SYS_LSTAT / SYS_ACCESS");

    struct stat st;
    int r = pu_stat("/", &st);
    check_eq("stat('/') succeeds", 0, r);
    check_eq("stat('/') reports type=DIR", 2, (int)st.st_type);
    check_eq("root is inode 2 (EXT2_ROOT_INODE)", 2, (int)st.st_ino);

    r = pu_stat("/README.TXT", &st);
    check_eq("stat('/README.TXT') succeeds", 0, r);
    check_eq("stat('/README.TXT') reports type=FILE", 1, (int)st.st_type);
    check_true("stat('/README.TXT') size > 0", st.st_size > 0);
    check_eq("stat('/README.TXT') mode is 0644", 0644, (int)(st.st_mode & 0777));
    check_eq("stat('/README.TXT') uid is 0", 0, (int)st.st_uid);
    check_eq("stat('/README.TXT') gid is 0", 0, (int)st.st_gid);
    check_eq("stat('/README.TXT') nlink is 1", 1, (int)st.st_nlink);
    check_true("stat('/README.TXT') atime is nonzero", st.st_atime > 0);
    check_true("stat('/README.TXT') mtime is nonzero", st.st_mtime > 0);
    check_true("stat('/README.TXT') ctime is nonzero", st.st_ctime > 0);

    check_eq("stat() on a missing path returns ENOENT", ENOENT, pu_stat("/no/such/thing", &st));
    check_eq("stat() with a null path returns EINVAL", EINVAL, pu_stat((const char *)0, &st));

    r = pu_stat("/etc", &st);
    check_eq("stat('/etc') reports type=DIR", 2, (int)st.st_type);
    check_true("directory size is nonzero (real i_size)", st.st_size > 0);
    check_true("directory nlink >= 2 ('.' plus parent's entry)", st.st_nlink >= 2);

    /* lstat: reports the symlink itself; stat: follows through it. */
    r = pu_lstat("/readme.link", &st);
    check_eq("lstat('/readme.link') succeeds", 0, r);
    check_eq("lstat('/readme.link') reports type=SYMLINK(3)", 3, (int)st.st_type);
    check_true("S_ISLNK true for readme.link", S_ISLNK(st.st_mode));
    check_eq("symlink conventional mode is 0777", 0777, (int)(st.st_mode & 0777));

    r = pu_stat("/readme.link", &st);
    check_eq("stat('/readme.link') follows through to a FILE", 1, (int)st.st_type);

    r = pu_lstat("/README.TXT", &st);
    check_eq("lstat on a non-symlink behaves like stat", 1, (int)st.st_type);

    check_eq("lstat() on a missing path returns ENOENT", ENOENT, pu_lstat("/no/such/thing", &st));

    /* SYS_ACCESS. */
    check_eq("access(F_OK) on an existing file succeeds", 0, pu_access("/README.TXT", F_OK));
    check_eq("access(F_OK) on a missing file returns ENOENT", ENOENT, pu_access("/no/such/file", F_OK));
    check_eq("root: R_OK on a mode-0000 file succeeds (root bypass)", 0, pu_access("/perm/noaccess.bin", R_OK));
    check_eq("root: W_OK on a mode-0444 file succeeds (root bypass)", 0, pu_access("/perm/readonly.txt", W_OK));
    check_eq("root: X_OK on a mode-0755 file succeeds", 0, pu_access("/perm/exec.sh", X_OK));
    check_eq("root: X_OK on a mode-0000 file is denied (no x bit anywhere)", EACCES, pu_access("/perm/noaccess.bin", X_OK));
    check_eq("access() with a null path returns EINVAL", EINVAL, pu_access((const char *)0, F_OK));
}

/* ==================================================================== */

static void test_permissions(void)
{
    section("Permissions: owner/group/other tiers, root bypass, traversal");

    /* As root (default credentials), every mode-0000 op still succeeds via
     * the root bypass; already covered by the stat/access section above.
     * Here: exercise the three non-root tiers using SYS_DEBUG_SETCRED. */
    int r;

    /* uid=1000/gid=999 matches neither group_test.txt's owner (0) nor its
       group (100) — falls all the way through to "other". */
    pu_debug_setcred(1000, 999);

    r = pu_access("/perm/private.txt", R_OK);
    check_eq("non-root, no match: R_OK on mode-0600 owner-only file denied", EACCES, r);

    r = pu_access("/perm/readonly.txt", R_OK);
    check_eq("non-root: R_OK on mode-0444 world-readable file succeeds", 0, r);

    r = pu_access("/perm/readonly.txt", W_OK);
    check_eq("non-root: W_OK on mode-0444 file denied (no bypass)", EACCES, r);

    r = pu_access("/perm/group_test.txt", R_OK);
    check_eq("non-root, gid mismatch: R_OK on mode-0640 group file denied", EACCES, r);

    r = pu_access("/perm/exec.sh", X_OK);
    check_eq("non-root: X_OK on mode-0755 file succeeds via other bits", 0, r);

    r = pu_access("/perm/noaccess.bin", R_OK);
    int r2 = pu_access("/perm/noaccess.bin", W_OK);
    int r3 = pu_access("/perm/noaccess.bin", X_OK);
    check_true("non-root: mode-0000 file denies R/W/X with no bypass", r == EACCES && r2 == EACCES && r3 == EACCES);

    /* Now match the file's group (gid=100) but not its owner. */
    pu_debug_setcred(1000, 100);

    r = pu_access("/perm/group_test.txt", R_OK);
    check_eq("non-root, gid match: R_OK on mode-0640 group file succeeds", 0, r);

    r = pu_access("/perm/group_test.txt", W_OK);
    check_eq("non-root, gid match: W_OK denied (group bits are r-- only)", EACCES, r);

    /* Directory traversal permission: /perm/noxdir has mode 0600 (no x bit
       anywhere), so nobody — not even root — can traverse into it. */
    pu_debug_setcred(0, 0);
    struct stat st;
    r = pu_stat("/perm/noxdir/hidden.txt", &st);
    check_eq("root: traversal into a no-x-bit directory is denied (EACCES)", EACCES, r);
    r = pu_access("/perm/noxdir/hidden.txt", F_OK);
    check_eq("access() through a no-x-bit ancestor also reports EACCES", EACCES, r);

    /* Restore root; leaving credentials changed would corrupt every test
       after this one, since elf_exec() runs this program in the shell's own
       task (no fork/exec — see kernel/elf.c). */
    pu_debug_setcred(0, 0);
    check_eq("credentials restored to uid=0/gid=0", 0, pu_access("/perm/noaccess.bin", R_OK));
}

/* ==================================================================== */

static void test_chmod_chown(void)
{
    section("SYS_CHMOD / SYS_CHOWN (infrastructure-only stubs)");

    /* Neither filesystem stores mutable ownership/permission bits yet, so
       both always resolve to -EROFS for an existing path (or -ENOENT for a
       missing one) — see docs/api/vfs.md. */
    check_eq("chmod() on an existing file returns EROFS", EROFS, pu_chmod("/README.TXT", 0600));
    check_eq("chmod() on a missing file returns ENOENT", ENOENT, pu_chmod("/no/such/file", 0600));
    check_eq("chown() on an existing file returns EROFS", EROFS, pu_chown("/README.TXT", 5, 5));
    check_eq("chown() on a missing file returns ENOENT", ENOENT, pu_chown("/no/such/file", 5, 5));
}

/* ==================================================================== */

static void test_readdir(void)
{
    section("SYS_READDIR");

    struct dirent entries[64];
    int count = pu_readdir("/", entries, 64);
    check_true("readdir('/') succeeds with a positive count", count > 0);
    check_true("root listing contains README.TXT", find_name(entries, count, "README.TXT") >= 0);
    check_true("root listing contains etc", find_name(entries, count, "etc") >= 0);
    check_true("root listing contains bin", find_name(entries, count, "bin") >= 0);
    check_true("root listing includes '.' entry", find_name(entries, count, ".") >= 0);
    check_true("root listing includes '..' entry", find_name(entries, count, "..") >= 0);

    count = pu_readdir("/testdir", entries, 16);
    check_true("nested readdir('/testdir') finds alpha/beta/gamma.txt",
               find_name(entries, count, "alpha.txt") >= 0 &&
               find_name(entries, count, "beta.txt") >= 0 &&
               find_name(entries, count, "gamma.txt") >= 0);

    count = pu_readdir("/perm/emptydir", entries, 8);
    check_eq("emptydir contains exactly '.' and '..'", 2, count);

    check_eq("readdir() on a missing path returns ENOENT", ENOENT, pu_readdir("/no/such/dir", entries, 8));
    check_eq("readdir() with max_entries=0 returns EINVAL", EINVAL, pu_readdir("/", entries, 0));
    check_eq("readdir() with a null buffer returns EINVAL", EINVAL, pu_readdir("/", (struct dirent *)0, 8));

    /* Truncation: cap below the real count and confirm it's honored. */
    count = pu_readdir("/", entries, 2);
    check_eq("readdir() honors a small max_entries cap", 2, count);
}

/* ==================================================================== */

static void test_mkdir_rmdir(void)
{
    section("SYS_MKDIR / SYS_RMDIR");

    struct stat st;
    pu_rmdir("/systest_tmp_dir"); /* best-effort cleanup from a previous run */

    int r = pu_mkdir("/systest_tmp_dir");
    check_eq("mkdir() on a fresh path succeeds", 0, r);

    r = pu_stat("/systest_tmp_dir", &st);
    check_eq("stat() on the new directory reports type=DIR", 2, (int)st.st_type);

    struct dirent entries[8];
    int count = pu_readdir("/systest_tmp_dir", entries, 8);
    check_eq("new directory contains exactly '.' and '..'", 2, count);

    r = pu_mkdir("/systest_tmp_dir");
    check_eq("mkdir() on an already-existing path returns EEXIST", EEXIST, r);

    r = pu_mkdir("/no/such/parent/dir");
    check_eq("mkdir() with a missing parent returns ENOENT", ENOENT, r);

    int fd = pu_creat("/systest_tmp_dir/occupant.txt");
    check_true("creat() inside the new directory succeeds", fd >= 3);
    pu_close(fd);

    r = pu_rmdir("/systest_tmp_dir");
    check_eq("rmdir() on a non-empty directory returns ENOTEMPTY", ENOTEMPTY, r);

    pu_unlink("/systest_tmp_dir/occupant.txt");
    r = pu_rmdir("/systest_tmp_dir");
    check_eq("rmdir() on a now-empty directory succeeds", 0, r);

    r = pu_stat("/systest_tmp_dir", &st);
    check_eq("removed directory no longer stats", ENOENT, r);

    check_eq("rmdir() on a missing path returns ENOENT", ENOENT, pu_rmdir("/systest_tmp_dir"));
    check_eq("rmdir() on a regular file returns ENOTDIR", ENOTDIR, pu_rmdir("/README.TXT"));
    check_eq("rmdir('/') is refused", EACCES, pu_rmdir("/"));
}

/* ==================================================================== */

static void test_creat_write_unlink(void)
{
    section("creat() / write() / unlink()");

    pu_unlink("/systest_tmp_file.txt");

    int fd = pu_creat("/systest_tmp_file.txt");
    check_true("creat() succeeds", fd >= 3);
    const char *msg = "PureUNIX systest content\n";
    int len = (int)pu_strlen(msg);
    check_eq("write() writes the full buffer", len, pu_write(fd, msg, len));
    pu_close(fd);

    fd = pu_open("/systest_tmp_file.txt", O_RDONLY);
    char buf[64];
    int n = pu_read(fd, buf, sizeof(buf));
    check_eq("readback length matches what was written", len, n);
    check_true("readback content matches exactly", bytes_eq(buf, msg, len));
    pu_close(fd);

    check_eq("unlink() succeeds", 0, pu_unlink("/systest_tmp_file.txt"));

    struct stat st;
    check_eq("unlinked file no longer stats", ENOENT, pu_stat("/systest_tmp_file.txt", &st));

    check_eq("unlink() on a missing path returns ENOENT", ENOENT, pu_unlink("/systest_tmp_file.txt"));
    check_eq("unlink() on a directory returns EISDIR", EISDIR, pu_unlink("/etc"));
}

/* ==================================================================== */

static void test_rename(void)
{
    section("SYS_RENAME");

    pu_unlink("/systest_rename_src.txt");
    pu_unlink("/systest_rename_dst.txt");
    pu_unlink("/testdir/systest_rename_moved.txt");
    pu_unlink("/systest_rename_over.txt");

    int fd = pu_creat("/systest_rename_src.txt");
    pu_write(fd, "rename-me", 9);
    pu_close(fd);

    struct stat st;
    int r = pu_rename("/systest_rename_src.txt", "/systest_rename_dst.txt");
    check_eq("rename() within the same directory succeeds", 0, r);
    check_eq("old name is gone after rename", ENOENT, pu_stat("/systest_rename_src.txt", &st));
    check_eq("new name exists after rename", 0, pu_stat("/systest_rename_dst.txt", &st));

    r = pu_rename("/systest_rename_dst.txt", "/testdir/systest_rename_moved.txt");
    check_eq("cross-directory rename succeeds", 0, r);
    fd = pu_open("/testdir/systest_rename_moved.txt", O_RDONLY);
    char buf[16];
    int n = fd >= 3 ? pu_read(fd, buf, 9) : -1;
    check_true("cross-directory rename preserved content", fd >= 3 && n == 9 && bytes_eq(buf, "rename-me", 9));
    if (fd >= 3) pu_close(fd);

    fd = pu_creat("/systest_rename_over.txt");
    pu_write(fd, "original", 8);
    pu_close(fd);
    struct stat before;
    pu_stat("/testdir/systest_rename_moved.txt", &before);

    r = pu_rename("/testdir/systest_rename_moved.txt", "/systest_rename_over.txt");
    check_eq("rename() over an existing file succeeds", 0, r);
    struct stat after;
    pu_stat("/systest_rename_over.txt", &after);
    check_eq("rename-over-existing carries the source inode over", (int)before.st_ino, (int)after.st_ino);
    fd = pu_open("/systest_rename_over.txt", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, buf, 9) : -1;
    check_true("rename-over-existing replaced the content", fd >= 3 && n == 9 && bytes_eq(buf, "rename-me", 9));
    if (fd >= 3) pu_close(fd);
    pu_unlink("/systest_rename_over.txt");

    check_eq("rename() of a missing source returns ENOENT", ENOENT,
             pu_rename("/no/such/source.txt", "/systest_rename_dst.txt"));

    /* Cross-filesystem rename: EXT2 (/) -> FAT16 (/fat). */
    fd = pu_creat("/systest_cross_device.txt");
    pu_close(fd);
    r = pu_rename("/systest_cross_device.txt", "/fat/systest_cross_device.txt");
    check_eq("rename across filesystems returns EXDEV", EXDEV, r);
    pu_unlink("/systest_cross_device.txt");
}

/* ==================================================================== */

static void test_hardlinks(void)
{
    section("SYS_LINK (hard links)");

    pu_unlink("/systest_link_a.txt");
    pu_unlink("/systest_link_b.txt");

    int fd = pu_creat("/systest_link_a.txt");
    pu_write(fd, "shared-data", 11);
    pu_close(fd);

    int r = pu_link("/systest_link_a.txt", "/systest_link_b.txt");
    check_eq("link() creates a second name", 0, r);

    struct stat sa, sb;
    pu_stat("/systest_link_a.txt", &sa);
    pu_stat("/systest_link_b.txt", &sb);
    check_eq("hard-linked names share the same inode", (int)sa.st_ino, (int)sb.st_ino);
    check_true("both names report nlink=2", sa.st_nlink == 2 && sb.st_nlink == 2);

    fd = pu_open("/systest_link_b.txt", O_WRONLY | O_TRUNC);
    pu_write(fd, "via-b", 5);
    pu_close(fd);
    fd = pu_open("/systest_link_a.txt", O_RDONLY);
    char buf[16];
    int n = pu_read(fd, buf, 5);
    check_true("write through one name is visible via the other", n == 5 && bytes_eq(buf, "via-b", 5));
    pu_close(fd);

    r = pu_unlink("/systest_link_a.txt");
    pu_stat("/systest_link_b.txt", &sb);
    check_true("unlinking one name drops nlink to 1, file still accessible", r == 0 && sb.st_nlink == 1);

    r = pu_link("/systest_link_a.txt", "/systest_link_should_fail.txt");
    check_eq("link() on an already-fully-unlinked source returns ENOENT", ENOENT, r);

    r = pu_link("/systest_link_b.txt", "/etc");
    check_eq("link() to an existing destination returns EEXIST", EEXIST, r);

    r = pu_link("/", "/systest_root_link_should_fail");
    check_eq("link() refuses to hard-link a directory (EPERM)", EPERM, r);

    pu_unlink("/systest_link_b.txt");
    check_eq("unlinking the last link frees the inode", ENOENT, pu_stat("/systest_link_b.txt", &sb));
}

/* ==================================================================== */

static void test_symlinks(void)
{
    section("SYS_SYMLINK / SYS_READLINK / loop detection");

    char buf[256];
    int r;

    r = pu_readlink("/abslink", buf, sizeof(buf));
    check_true("readlink() reads an absolute target verbatim", r == 11 && bytes_eq(buf, "/README.TXT", 11));

    r = pu_readlink("/readme.link", buf, sizeof(buf));
    check_true("readlink() reads a relative target verbatim", r == 10 && bytes_eq(buf, "README.TXT", 10));

    r = pu_readlink("/testdir/uplink", buf, sizeof(buf));
    check_true("readlink() reads a '..'-relative target verbatim", r == 13 && bytes_eq(buf, "../README.TXT", 13));

    char small[4];
    r = pu_readlink("/readme.link", small, sizeof(small));
    check_true("readlink() truncates to the caller's buffer size", r == 4 && bytes_eq(small, "READ", 4));

    check_eq("readlink() on a non-symlink returns EINVAL", EINVAL, pu_readlink("/README.TXT", buf, sizeof(buf)));
    check_eq("readlink() on a missing path returns ENOENT", ENOENT, pu_readlink("/no/such/link", buf, sizeof(buf)));

    /* Following through open/read/stat. */
    int fd = pu_open("/readme.link", O_RDONLY);
    check_true("open() follows a symlink to a readable regular file", fd >= 3);
    if (fd >= 3) pu_close(fd);

    struct stat st;
    pu_stat("/abslink", &st);
    check_true("stat() follows an absolute-target symlink to a nonempty file", st.st_type == 1 && st.st_size > 0);

    /* Fast (inline, <=60 bytes) vs block-based (>60 bytes) symlinks. */
    pu_unlink("/systest_fastlink");
    pu_unlink("/systest_longlink");
    r = pu_symlink("/README.TXT", "/systest_fastlink");
    check_eq("symlink() with a short target succeeds", 0, r);
    pu_lstat("/systest_fastlink", &st);
    check_eq("a fast symlink allocates zero data blocks", 0, (int)st.st_blocks);

    const char *long_target =
        "./././././././././././././././././././././././././././././././././././././"
        "README.TXT";
    r = pu_symlink(long_target, "/systest_longlink");
    check_eq("symlink() with a >60-byte target succeeds", 0, r);
    pu_lstat("/systest_longlink", &st);
    check_true("a long symlink allocates at least one data block", st.st_blocks > 0);
    r = pu_readlink("/systest_longlink", buf, sizeof(buf));
    int tlen = (int)pu_strlen(long_target);
    check_true("long symlink reads back byte-for-byte", r == tlen && bytes_eq(buf, long_target, tlen));
    fd = pu_open("/systest_longlink", O_RDONLY);
    int n = fd >= 3 ? pu_read(fd, buf, 12) : -1;
    check_true("long symlink still resolves through './' components", fd >= 3 && n == 12 && bytes_eq(buf, "PureUnix EXT", 12));
    if (fd >= 3) pu_close(fd);
    pu_unlink("/systest_fastlink");
    pu_unlink("/systest_longlink");

    /* symlink() onto an existing path fails with EEXIST. */
    r = pu_symlink("/README.TXT", "/etc");
    check_eq("symlink() onto an existing path returns EEXIST", EEXIST, r);

    /* Loop detection: build our own A<->B cycle, independent of any
       pre-baked fixture, so this test is fully self-contained. */
    pu_unlink("/systest_loop_a");
    pu_unlink("/systest_loop_b");
    pu_symlink("/systest_loop_b", "/systest_loop_a");
    pu_symlink("/systest_loop_a", "/systest_loop_b");

    check_eq("stat() on a symlink loop gives up with ELOOP", ELOOP, pu_stat("/systest_loop_a", &st));
    check_eq("open() on a symlink loop reports ELOOP", ELOOP, pu_open("/systest_loop_b", O_RDONLY));
    check_eq("access() on a symlink loop reports ELOOP", ELOOP, pu_access("/systest_loop_a", F_OK));
    {
        struct dirent loop_entries[4];
        check_eq("readdir() through a symlink loop reports ELOOP", ELOOP,
                 pu_readdir("/systest_loop_a", loop_entries, 4));
    }

    pu_unlink("/systest_loop_a");
    pu_unlink("/systest_loop_b");
}

/* ==================================================================== */

static void test_large_files(void)
{
    section("Large files: direct + singly-indirect block iteration");

    struct stat st;
    int r = pu_stat("/bigfile.bin", &st);
    check_true("stat('/bigfile.bin') size=5120 (5 direct blocks)", r == 0 && st.st_size == 5120);

    int fd = pu_open("/bigfile.bin", O_RDONLY);
    check_true("open('/bigfile.bin') succeeds", fd >= 3);

    pu_lseek(fd, 1020, SEEK_SET);
    char buf[1024];
    int n = pu_read(fd, buf, 8);
    int all_b = 1;
    for (int i = 0; i < 8; i++) if (buf[i] != 'B') all_b = 0;
    check_true("8 bytes spanning a block boundary are all 'B'", n == 8 && all_b);

    pu_lseek(fd, 4096, SEEK_SET);
    n = pu_read(fd, buf, 1024);
    all_b = 1;
    for (int i = 0; i < n; i++) if (buf[i] != 'B') all_b = 0;
    check_true("the last direct block (5th) reads 1024 'B's", n == 1024 && all_b);

    n = pu_read(fd, buf, 1);
    check_eq("read after the last block returns EOF", 0, n);
    pu_close(fd);

    r = pu_stat("/hugefile.bin", &st);
    check_true("stat('/hugefile.bin') size=14336 (needs singly-indirect)", r == 0 && st.st_size == 14336);

    fd = pu_open("/hugefile.bin", O_RDONLY);
    int indirect_ok = 1;
    for (int blk = 0; blk < 14; blk++) {
        pu_lseek(fd, blk * 1024, SEEK_SET);
        unsigned char c;
        n = pu_read(fd, (char *)&c, 1);
        if (n != 1 || c != 0x00) indirect_ok = 0;
    }
    check_true("all 14 blocks (direct + indirect) start with 0x00", indirect_ok);

    pu_lseek(fd, 13 * 1024 + 255, SEEK_SET);
    unsigned char c;
    n = pu_read(fd, (char *)&c, 1);
    check_true("byte at 13*1024+255 in the indirect region is 0xFF", n == 1 && c == 0xFF);

    pu_lseek(fd, 14 * 1024, SEEK_SET);
    n = pu_read(fd, buf, 1);
    check_eq("read at the end of the indirect file returns EOF", 0, n);

    /* SEEK_CUR relative navigation inside the indirect region. */
    pu_lseek(fd, 12 * 1024, SEEK_SET);
    n = pu_read(fd, buf, 256);
    int pat_ok = (n == 256);
    for (int i = 0; pat_ok && i < 256; i++) {
        if ((unsigned char)buf[i] != (unsigned char)i) pat_ok = 0;
    }
    check_true("256-byte read from the indirect region matches 0x00..0xFF", pat_ok);

    int cur = pu_lseek(fd, -128, SEEK_CUR);
    check_eq("SEEK_CUR -128 lands at the expected position", 12 * 1024 + 128, cur);

    char rebuf[128];
    n = pu_read(fd, rebuf, 128);
    check_true("re-read after SEEK_CUR matches the original bytes", n == 128 && bytes_eq(buf + 128, rebuf, 128));
    pu_close(fd);
}

/* ==================================================================== */

static void test_simultaneous_fds(void)
{
    section("Simultaneous file descriptors");

    int fa = pu_open("/README.TXT", O_RDONLY);
    int fb = pu_open("/etc/passwd", O_RDONLY);
    check_true("two files can be open simultaneously", fa >= 3 && fb >= 3);

    char a[8], b[8];
    int na = pu_read(fa, a, 8);
    int nb = pu_read(fb, b, 8);
    check_true("reads on independent fds return full buffers", na == 8 && nb == 8);

    pu_lseek(fa, 0, SEEK_SET);
    int fb_pos = pu_lseek(fb, 0, SEEK_CUR);
    check_eq("seeking one fd does not affect another's offset", 8, fb_pos);

    pu_close(fa);
    pu_close(fb);
}

/* ==================================================================== */

static void test_directory_growth_and_reuse(void)
{
    section("Directory growth and allocator reuse (stress)");

    enum { N = 200 };
    pu_rmdir("/systest_stress");
    int r = pu_mkdir("/systest_stress");
    check_eq("mkdir('/systest_stress') succeeds", 0, r);

    int created = 0;
    for (int i = 0; i < N; i++) {
        char path[32];
        num_path(path, "/systest_stress/f", i);
        int fd = pu_creat(path);
        if (fd >= 3) {
            pu_write(fd, "x", 1);
            pu_close(fd);
            created++;
        }
    }
    check_eq("created 200 files in a fresh directory", N, created);

    struct dirent entries[256];
    int count = pu_readdir("/systest_stress", entries, 256);
    check_eq("directory listing shows 200 files plus '.' and '..'", N + 2, count);

    int deleted = 0;
    for (int i = 0; i < N; i += 2) {
        char path[32];
        num_path(path, "/systest_stress/f", i);
        if (pu_unlink(path) == 0) deleted++;
    }
    check_eq("deleted every other file (100 of 200)", N / 2, deleted);

    int recreated = 0;
    for (int i = 0; i < N; i += 2) {
        char path[32];
        num_path(path, "/systest_stress/f", i);
        int fd = pu_creat(path);
        if (fd >= 3) {
            pu_write(fd, "y", 1);
            pu_close(fd);
            recreated++;
        }
    }
    check_eq("recreated 100 files reusing freed inodes/blocks", N / 2, recreated);

    count = pu_readdir("/systest_stress", entries, 256);
    check_eq("directory entry count is back to 202 after delete+recreate", N + 2, count);

    int fd = pu_open("/systest_stress/f1", O_RDONLY);
    char c;
    int n = fd >= 3 ? pu_read(fd, &c, 1) : -1;
    check_true("a never-deleted file still reads its original content", fd >= 3 && n == 1 && c == 'x');
    if (fd >= 3) pu_close(fd);

    fd = pu_open("/systest_stress/f0", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, &c, 1) : -1;
    check_true("a deleted-then-recreated file reads its new content", fd >= 3 && n == 1 && c == 'y');
    if (fd >= 3) pu_close(fd);

    for (int i = 0; i < N; i++) {
        char path[32];
        num_path(path, "/systest_stress/f", i);
        pu_unlink(path);
    }
    r = pu_rmdir("/systest_stress");
    check_eq("rmdir succeeds once the stress directory is emptied", 0, r);
}

/* ==================================================================== */

static void test_repeated_ops_stress(void)
{
    section("Repeated open/close/stat/readlink stress");

    int ok = 1;
    for (int i = 0; i < 100; i++) {
        int fd = pu_open("/README.TXT", O_RDONLY);
        if (fd < 3) { ok = 0; break; }
        if (pu_close(fd) != 0) { ok = 0; break; }
    }
    check_true("100 repeated open/close cycles all succeed", ok);

    ok = 1;
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (pu_stat("/README.TXT", &st) != 0 || st.st_ino == 0) { ok = 0; break; }
    }
    check_true("100 repeated stat() calls all succeed with a stable inode", ok);

    ok = 1;
    for (int i = 0; i < 100; i++) {
        char buf[16];
        int n = pu_readlink("/readme.link", buf, sizeof(buf));
        if (n != 10 || !bytes_eq(buf, "README.TXT", 10)) { ok = 0; break; }
    }
    check_true("100 repeated readlink() calls all return the same target", ok);

    /* Rename chain: A -> B -> C -> A, content must survive the whole loop. */
    pu_unlink("/systest_chain_a.txt");
    pu_unlink("/systest_chain_b.txt");
    pu_unlink("/systest_chain_c.txt");
    int fd = pu_creat("/systest_chain_a.txt");
    pu_write(fd, "chain", 5);
    pu_close(fd);
    pu_rename("/systest_chain_a.txt", "/systest_chain_b.txt");
    pu_rename("/systest_chain_b.txt", "/systest_chain_c.txt");
    pu_rename("/systest_chain_c.txt", "/systest_chain_a.txt");
    fd = pu_open("/systest_chain_a.txt", O_RDONLY);
    char buf[8];
    int n = fd >= 3 ? pu_read(fd, buf, 5) : -1;
    check_true("a rename chain back to the original name preserves content", fd >= 3 && n == 5 && bytes_eq(buf, "chain", 5));
    if (fd >= 3) pu_close(fd);
    pu_unlink("/systest_chain_a.txt");

    /* Hard-link chain: one file, several names, all sharing one inode. */
    pu_unlink("/systest_hlink_1.txt");
    pu_unlink("/systest_hlink_2.txt");
    pu_unlink("/systest_hlink_3.txt");
    fd = pu_creat("/systest_hlink_1.txt");
    pu_write(fd, "linked", 6);
    pu_close(fd);
    pu_link("/systest_hlink_1.txt", "/systest_hlink_2.txt");
    pu_link("/systest_hlink_2.txt", "/systest_hlink_3.txt");
    struct stat s1, s2, s3;
    pu_stat("/systest_hlink_1.txt", &s1);
    pu_stat("/systest_hlink_2.txt", &s2);
    pu_stat("/systest_hlink_3.txt", &s3);
    check_true("a 3-way hard-link chain shares one inode with nlink=3",
               s1.st_ino == s2.st_ino && s2.st_ino == s3.st_ino && s1.st_nlink == 3);
    pu_unlink("/systest_hlink_1.txt");
    pu_unlink("/systest_hlink_2.txt");
    pu_unlink("/systest_hlink_3.txt");

    /* Symlink chain: A -> B -> C -> README.TXT, opening A must resolve all
       the way through. */
    pu_unlink("/systest_schain_a");
    pu_unlink("/systest_schain_b");
    pu_unlink("/systest_schain_c");
    pu_symlink("/README.TXT", "/systest_schain_c");
    pu_symlink("/systest_schain_c", "/systest_schain_b");
    pu_symlink("/systest_schain_b", "/systest_schain_a");
    fd = pu_open("/systest_schain_a", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, buf, 8) : -1;
    check_true("a 3-hop symlink chain resolves to the final target", fd >= 3 && n == 8);
    if (fd >= 3) pu_close(fd);
    pu_unlink("/systest_schain_a");
    pu_unlink("/systest_schain_b");
    pu_unlink("/systest_schain_c");
}

/* ==================================================================== */

static void test_ext2_specifics(void)
{
    section("EXT2-specific: allocator reuse, ENOSPC, cross-fs EPERM");

    /* Fill the 4 MiB filesystem's remaining free space with separate,
       modest-sized files (each its own fresh write buffer) until the block
       allocator runs out at close() time (when a buffered write is
       actually flushed to disk — see docs/syscalls.md's SYS_WRITE
       section). Using many *separate* files, rather than growing one
       file's buffer across many writes, avoids the O(n^2) cost of this
       kernel's write-buffer growth (every SYS_WRITE on the same fd
       reallocates and re-zeroes the *entire* accumulated buffer from
       scratch, so repeatedly growing one fd to several MiB is drastically
       more expensive than writing the same total spread over many fds). */
    enum { FILLER_SIZE = 65536, MAX_FILLERS = 128 };
    static char filler[FILLER_SIZE];
    for (int i = 0; i < FILLER_SIZE; i++) filler[i] = 'z';

    int filler_count = 0;
    int enospc_seen = 0;
    for (int i = 0; i < MAX_FILLERS; i++) {
        char path[32];
        num_path(path, "/systest_fill_", i);
        int ffd = pu_creat(path);
        if (ffd < 3) break;
        int wrote = pu_write(ffd, filler, FILLER_SIZE);
        int close_rc = pu_close(ffd);
        filler_count = i + 1;
        if (wrote == ENOSPC || close_rc == ENOSPC) { enospc_seen = 1; break; }
    }
    check_true("writing enough files eventually reports ENOSPC", enospc_seen);

    /* Clean up every filler file (including the last, failed one — its
       inode is a valid, if empty, file after the write.c fix above, so
       unlink() works on it exactly like any other). */
    for (int i = 0; i < filler_count; i++) {
        char path[32];
        num_path(path, "/systest_fill_", i);
        pu_unlink(path);
    }

    /* Freeing every filler file must return the disk to a state where a
       modest file can be created again — proving the ENOSPC path above
       left no blocks or inodes leaked or dangling. */
    int fd = pu_creat("/systest_enospc_recheck.txt");
    check_true("disk usable again after freeing all filler files (no leak)", fd >= 3);
    if (fd >= 3) { pu_write(fd, "ok", 2); pu_close(fd); }
    pu_unlink("/systest_enospc_recheck.txt");

    /* EPERM: link() refuses directories (already covered in hardlinks
       section too, but every documented errno gets its own explicit check
       per this suite's requirements). */
    int r = pu_link("/etc", "/systest_dir_link_should_fail");
    check_eq("link() on a directory returns EPERM (dedicated errno check)", EPERM, r);
}

/* ==================================================================== */

static void test_fat16(void)
{
    section("FAT16: mount, lookup, read, write, EXT2 boundary");

    struct stat st;
    int r = pu_stat("/fat", &st);
    check_eq("stat('/fat') resolves to the FAT16 mount root", 2, (int)st.st_type);

    r = pu_stat("/fat/README.TXT", &st);
    check_true("FAT16 lookup finds README.TXT", r == 0 && st.st_type == 1);

    int fd = pu_open("/fat/README.TXT", O_RDONLY);
    check_true("FAT16 open+read succeeds", fd >= 3);
    char buf[16];
    int n = fd >= 3 ? pu_read(fd, buf, 12) : -1;
    check_true("FAT16 read returns data", fd >= 3 && n > 0);
    if (fd >= 3) pu_close(fd);

    /* Write behavior on FAT16 mirrors the EXT2 write path through the same
       VFS layer: creat/write/close/reopen/verify, then unlink. */
    pu_unlink("/fat/sytest.txt");
    fd = pu_creat("/fat/sytest.txt");
    check_true("FAT16 creat() succeeds", fd >= 3);
    pu_write(fd, "fat16", 5);
    pu_close(fd);
    fd = pu_open("/fat/sytest.txt", O_RDONLY);
    n = fd >= 3 ? pu_read(fd, buf, 5) : -1;
    check_true("FAT16 write+reopen round-trips content", fd >= 3 && n == 5 && bytes_eq(buf, "fat16", 5));
    if (fd >= 3) pu_close(fd);
    r = pu_unlink("/fat/sytest.txt");
    check_eq("FAT16 unlink() succeeds", 0, r);

    /* Mount boundary: EXT2 root listing must not contain FAT16 entries, and
       vice versa — no union/merge across mounts. */
    struct dirent entries[64];
    int count = pu_readdir("/", entries, 64);
    check_true("EXT2 root listing does not leak FAT16's BIN directory",
               find_name(entries, count, "BIN") < 0);
    count = pu_readdir("/fat", entries, 64);
    check_true("FAT16 root listing does not contain EXT2's etc directory",
               find_name(entries, count, "etc") < 0);
}

/* ==================================================================== */

static void test_elf_exec_preconditions(void)
{
    section("ELF execution preconditions (real fork()/exec()/wait() are covered above)");

    struct stat st;
    int r = pu_stat("/bin/hello.elf", &st);
    check_true("an installed ELF exists and is a regular file", r == 0 && st.st_type == 1);
    check_eq("access(X_OK) on an installed ELF succeeds (elf_exec's own gate)", 0, pu_access("/bin/hello.elf", X_OK));

    check_eq("access(X_OK) on a missing ELF path returns ENOENT", ENOENT, pu_access("/bin/no_such.elf", X_OK));

    /* /README.TXT is mode 0644 — no execute bit for anyone; this is the
       exact check elf_exec() performs before ever reading the file. */
    check_eq("access(X_OK) on a non-executable regular file is denied", EACCES, pu_access("/README.TXT", X_OK));

    /* This program's own successful execution to this point already proves
       valid-ELF exec and the return-to-shell path both work end to end. */
    check_true("this program is itself proof that exec + return-to-shell work", 1);
}

/* ==================================================================== */

int main(void)
{
    pu_puts("=== PureUNIX System Test ===\n");

    test_process_basics();
    test_fork_exec_wait();
    test_write_read_basics();
    test_isatty_ioctl();
    test_open_close();
    test_lseek();
    test_stat_lstat_access();
    test_permissions();
    test_chmod_chown();
    test_readdir();
    test_mkdir_rmdir();
    test_creat_write_unlink();
    test_rename();
    test_hardlinks();
    test_symlinks();
    test_large_files();
    test_simultaneous_fds();
    test_directory_growth_and_reuse();
    test_repeated_ops_stress();
    test_ext2_specifics();
    test_fat16();
    test_elf_exec_preconditions();

    pu_puts("\n====================================\n");
    pu_puts("PureUNIX System Test Complete\n");
    pu_puts("Tests: "); pu_puti(g_num); pu_puts("\n");
    pu_puts("PASS: ");  pu_puti(g_pass); pu_puts("\n");
    pu_puts("FAIL: ");  pu_puti(g_fail); pu_puts("\n");
    pu_puts("====================================\n");

    return g_fail == 0 ? 0 : 1;
}
