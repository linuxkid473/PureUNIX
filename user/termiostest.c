/*
 * user/termiostest.c — regression + interactive demo for PureUNIX's
 * termios implementation (SYS_TCGETATTR / SYS_TCSETATTR, drivers/tty.c).
 *
 * Follows the same harness pattern as user/systest.c: independent, numbered,
 * non-fatal checks with a final PASS/FAIL summary. Section one is fully
 * deterministic and non-blocking (safe to run unattended). Section two
 * actually reads from the keyboard to demonstrate canonical-vs-raw and
 * echo-vs-noecho behavior live; it requires someone at the console typing.
 */
#include "libpure.h"

static int g_num  = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    pu_puts("[");
    pu_puti(g_num);
    pu_puts("] ");
    pu_puts(desc);
    pu_puts("\n");
}

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

static void check_true(const char *desc, int cond)
{
    check_eq(desc, 1, cond ? 1 : 0);
}

static void section(const char *title)
{
    pu_puts("\n--- ");
    pu_puts(title);
    pu_puts(" ---\n");
}

/* ==================================================================== */

static void test_defaults(struct termios *orig)
{
    section("tcgetattr(): default console settings");

    struct termios t;
    check_eq("tcgetattr(0, ...) succeeds", 0, pu_tcgetattr(0, &t));

    check_true("default mode is canonical (ICANON set)", (t.c_lflag & ICANON) != 0);
    check_true("default mode echoes input (ECHO set)", (t.c_lflag & ECHO) != 0);
    check_true("default mode visually erases on backspace (ECHOE set)", (t.c_lflag & ECHOE) != 0);
    check_true("default mode delivers VINTR (ISIG set)", (t.c_lflag & ISIG) != 0);

    check_eq("default VINTR is ^C (3)", 3, t.c_cc[VINTR]);
    check_eq("default VEOF is ^D (4)", 4, t.c_cc[VEOF]);
    check_eq("default VERASE is DEL (127)", 127, t.c_cc[VERASE]);
    check_eq("default VKILL is ^U (21)", 21, t.c_cc[VKILL]);
    check_eq("default VMIN is 1", 1, t.c_cc[VMIN]);
    check_eq("default VTIME is 0", 0, t.c_cc[VTIME]);

    struct termios t1, t2;
    check_eq("tcgetattr(1, ...) succeeds (stdout names the same console)", 0, pu_tcgetattr(1, &t1));
    check_eq("tcgetattr(2, ...) succeeds (stderr names the same console)", 0, pu_tcgetattr(2, &t2));
    check_true("fd 0/1/2 report identical termios (one shared console)",
               t.c_lflag == t1.c_lflag && t1.c_lflag == t2.c_lflag);

    *orig = t;
}

static void test_error_paths(void)
{
    section("tcgetattr()/tcsetattr(): error paths");

    struct termios t;
    check_eq("tcgetattr with a null buffer returns EINVAL", EINVAL, pu_tcgetattr(0, (struct termios *)0));
    check_eq("tcsetattr with a null buffer returns EINVAL", EINVAL, pu_tcsetattr(0, TCSANOW, (struct termios *)0));

    check_eq("tcgetattr on a negative fd returns EBADF", EBADF, pu_tcgetattr(-1, &t));
    check_eq("tcgetattr on a never-opened fd returns EBADF", EBADF, pu_tcgetattr(9, &t));
    check_eq("tcsetattr on a never-opened fd returns EBADF", EBADF, pu_tcsetattr(9, TCSANOW, &t));

    int fd = pu_open("/README.TXT", O_RDONLY);
    check_true("opened /README.TXT to use as a non-tty fd", fd >= 3);
    if (fd >= 3) {
        check_eq("tcgetattr on an open regular file returns ENOTTY", ENOTTY, pu_tcgetattr(fd, &t));
        check_eq("tcsetattr on an open regular file returns ENOTTY", ENOTTY, pu_tcsetattr(fd, TCSANOW, &t));
        pu_close(fd);
    }

    check_eq("tcgetattr(0, ...) succeeds (fetch a valid buffer for the next check)", 0, pu_tcgetattr(0, &t));
    check_eq("tcsetattr with an invalid 'actions' argument returns EINVAL", EINVAL, pu_tcsetattr(0, 999, &t));
}

static void test_set_get_roundtrip(const struct termios *orig)
{
    section("tcsetattr(): flags and c_cc actually persist");

    struct termios raw = *orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    check_eq("tcsetattr(TCSANOW) switching to raw+noecho succeeds", 0, pu_tcsetattr(0, TCSANOW, &raw));

    struct termios back;
    check_eq("tcgetattr() after the switch succeeds", 0, pu_tcgetattr(0, &back));
    check_true("ICANON is now clear", (back.c_lflag & ICANON) == 0);
    check_true("ECHO is now clear", (back.c_lflag & ECHO) == 0);
    check_true("ISIG is untouched by the change", (back.c_lflag & ISIG) == (orig->c_lflag & ISIG));
    check_eq("c_cc is untouched by the change (VERASE)", orig->c_cc[VERASE], back.c_cc[VERASE]);

    struct termios custom = back;
    custom.c_cc[VERASE] = 8; /* ^H instead of the default DEL */
    check_eq("tcsetattr() changing a c_cc entry succeeds", 0, pu_tcsetattr(0, TCSAFLUSH, &custom));
    struct termios after_cc;
    pu_tcgetattr(0, &after_cc);
    check_eq("the changed c_cc entry persists across a get", 8, after_cc.c_cc[VERASE]);

    /* Restore exactly what test_defaults() first observed, so the shell the
     * user drops back into after this program exits still behaves normally
     * (this kernel has one shared console termios, not per-process state). */
    check_eq("restoring the original termios succeeds", 0, pu_tcsetattr(0, TCSANOW, orig));
    struct termios restored;
    pu_tcgetattr(0, &restored);
    check_true("restored termios matches the original", restored.c_lflag == orig->c_lflag &&
               restored.c_cc[VERASE] == orig->c_cc[VERASE]);
}

/* ==================================================================== */

static void demo_read_line(const char *prompt, char *buf, int cap)
{
    pu_puts(prompt);
    int n = pu_read(0, buf, cap - 1);
    if (n < 0) {
        pu_puts("  read() returned error "); pu_puti(n); pu_puts("\n");
        return;
    }
    buf[n] = '\0';
    pu_puts("  read() returned "); pu_puti(n); pu_puts(" bytes: \"");
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') { pu_puts("\\n"); continue; }
        pu_write(1, &c, 1);
    }
    pu_puts("\"\n");
}

static void interactive_demo(const struct termios *orig)
{
    section("Interactive demo (requires someone typing at the console)");
    char buf[128];

    pu_puts("Canonical mode (default): the tty echoes what you type and line-\n"
            "edits it (backspace works) until you press Enter.\n");
    demo_read_line("Type something and press Enter: ", buf, sizeof(buf));

    struct termios raw = *orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    pu_tcsetattr(0, TCSANOW, &raw);

    pu_puts("\nNow in raw mode with ECHO off: read() returns as soon as a few\n"
            "bytes are available, and nothing you type is echoed automatically\n"
            "(this program echoes it back manually below).\n");
    demo_read_line("Press a few keys: ", buf, sizeof(buf));

    pu_tcsetattr(0, TCSANOW, orig);
    pu_puts("\nRestored canonical echoing mode.\n");
}

/* ==================================================================== */

int main(void)
{
    pu_puts("=== PureUNIX termios Test ===\n");

    struct termios orig;
    test_defaults(&orig);
    test_error_paths();
    test_set_get_roundtrip(&orig);

    pu_puts("\n====================================\n");
    pu_puts("termios Test Complete\n");
    pu_puts("Tests: "); pu_puti(g_num); pu_puts("\n");
    pu_puts("PASS: ");  pu_puti(g_pass); pu_puts("\n");
    pu_puts("FAIL: ");  pu_puti(g_fail); pu_puts("\n");
    pu_puts("====================================\n");

    /* Unlike the checks above, this blocks on real keystrokes — it's here to
     * demonstrate (not machine-verify) canonical-vs-raw and echo-vs-noecho
     * behavior to whoever is running this from the console. */
    interactive_demo(&orig);

    return g_fail == 0 ? 0 : 1;
}
