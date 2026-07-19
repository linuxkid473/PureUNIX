/*
 * user/menucachetest.c — real on-target regression test for the
 * vendored MenuCache cross-build (third_party/menu-cache/i686-elf,
 * tools/build-menu-cache.sh). Fourth new dependency for the PCManFM-Qt
 * port (docs/pcmanfm-port.md phase 6) — libfm-qt's "Applications"
 * XDG-menu view is built on it.
 *
 * Exercises real menu-cache-gen (fork()+exec(), not libmenu-cache.a's
 * own client API — see this test's own header note below on why) against
 * a real, minimal XDG applications.menu +.desktop file
 * (third_party/menu-cache/pureunix-fixtures/), genuinely parsing real
 * XML/desktop-entry syntax and writing a real cache file whose content
 * this test verifies mentions the real app name/Exec line — not just a
 * link/exit-code check, the same class of on-target check every other
 * vendored dependency in this repo gets (see user/libctest.c/
 * user/glibtest.c).
 *
 * Real, disclosed scope limitation (not tested here, deliberately):
 * libmenu-cache.a's own client API (menu_cache_lookup()/_sync()) relies
 * on a persistent background GThread (server_io_thread(), reading the
 * daemon's socket in an infinite loop for the lifetime of the client
 * process) for its designed connection-management pattern — traced
 * directly in the real upstream source, not guessed. This platform's
 * pthread shim (user/newlib_syscalls.c) runs pthread_create()'s entry
 * point synchronously inline (a deliberate, disclosed design given no
 * real preemptive threading exists on this kernel — see
 * docs/pcmanfm-port.md's own pthread-shim writeup), so calling that API
 * would hang forever inside g_thread_new()'s own synchronous execution
 * of that infinite read loop. The real menu-cache-daemon
 * (/usr/libexec/menu-cache/menu-cached) and the real AF_UNIX socket
 * primitive it needs (kernel/unix_socket.c) both build and are verified
 * independently (user/unixsocktest.c) — only the convenience client
 * wrapper's own threading assumption is the real, disclosed gap. Same
 * harness convention as systest.c/libctest.c: every check is independent
 * and numbered, a failure never stops the run, a summary prints at the
 * end.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    printf("[%03d] %s\n", g_num, desc);
}

static void t_check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("      PASS: %s\n", what);
    } else {
        g_fail++;
        printf("      FAIL: %s\n", what);
    }
}

#define GEN_PATH "/usr/libexec/menu-cache/menu-cache-gen"
#define MENU_PATH "/etc/xdg/menus/applications.menu"
#define OUT_PATH "/tmp/menucachetest-output"

int main(void)
{
    t_begin("real files present: menu-cache-gen ELF + real XDG fixtures");
    struct stat st;
    t_check(stat(GEN_PATH, &st) == 0, "menu-cache-gen exists at its real, compiled-in libexec path");
    t_check(stat(MENU_PATH, &st) == 0, "a real applications.menu fixture exists");
    t_check(stat("/usr/share/applications/pureunix-test-app.desktop", &st) == 0,
            "a real .desktop file exists under /usr/share/applications");

    t_begin("fork()+exec() menu-cache-gen: real XDG menu parsing, not a link check");
    unlink(OUT_PATH);
    pid_t pid = fork();
    t_check(pid >= 0, "fork() succeeds");

    if (pid == 0) {
        execl(GEN_PATH, GEN_PATH, "-i", MENU_PATH, "-o", OUT_PATH, (char *)NULL);
        _exit(127); /* only reached if execl() itself failed */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    t_check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "menu-cache-gen exits 0 (real successful generation)");

    t_begin("real generated cache file content");
    int fd = open(OUT_PATH, O_RDONLY);
    t_check(fd >= 0, "the real output cache file was created");
    if (fd >= 0) {
        char buf[4096];
        memset(buf, 0, sizeof(buf));
        int n = read(fd, buf, sizeof(buf) - 1);
        t_check(n > 0, "the cache file has real, non-empty content");
        t_check(strstr(buf, "PureUnix Test App") != NULL,
                "the cache genuinely contains our real .desktop entry's Name");
        t_check(strstr(buf, "/bin/hello.elf") != NULL,
                "the cache genuinely contains our real .desktop entry's Exec");
        close(fd);
    } else {
        t_check(0, "the cache file has real, non-empty content");
        t_check(0, "the cache genuinely contains our real .desktop entry's Name");
        t_check(0, "the cache genuinely contains our real .desktop entry's Exec");
    }
    unlink(OUT_PATH);

    printf("\nmenucachetest: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
