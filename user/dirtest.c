/*
 * user/dirtest.c — regression test for the newlib DIR* stream API
 * (user/newlib_compat/dirent.h, opendir()/readdir()/closedir() in
 * user/newlib_syscalls.c) built over PureUNIX's cursor-less SYS_READDIR.
 *
 * Same harness convention as systest.c/libctest.c/exectest.c: numbered
 * checks, a failure never stops the run, summary at the end.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void check(const char *desc, int ok)
{
    g_num++;
    printf("[%03d] %s: %s\n", g_num, desc, ok ? "PASS" : "FAIL");
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

int main(void)
{
    printf("=== PureUNIX DIR* stream test ===\n");

    /* Fresh test directory with one file and one subdirectory child, so
     * '.', '..', the file, and the subdirectory are all exercised. */
    rmdir("/dirtest_sub_dir");
    unlink("/dirtest_dir/afile.txt");
    rmdir("/dirtest_dir");
    check("mkdir() for the test directory succeeds", mkdir("/dirtest_dir", 0755) == 0);
    check("mkdir() for a nested subdirectory succeeds", mkdir("/dirtest_dir/sub", 0755) == 0);
    FILE *f = fopen("/dirtest_dir/afile.txt", "w");
    check("creating a file inside the test directory succeeds", f != NULL);
    if (f) {
        fputs("hello\n", f);
        fclose(f);
    }

    DIR *d = opendir("/dirtest_dir");
    check("opendir() on a real directory succeeds", d != NULL);

    if (d) {
        int dot = 0, dotdot = 0, file_seen = 0, sub_seen = 0;
        unsigned int file_type = 999, sub_type = 999;
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) != NULL) {
            n++;
            if (strcmp(e->d_name, ".") == 0) dot = 1;
            else if (strcmp(e->d_name, "..") == 0) dotdot = 1;
            else if (strcmp(e->d_name, "afile.txt") == 0) { file_seen = 1; file_type = e->d_type; }
            else if (strcmp(e->d_name, "sub") == 0) { sub_seen = 1; sub_type = e->d_type; }
        }
        check("readdir() enumerated exactly 4 entries (., .., afile.txt, sub)", n == 4);
        check("readdir() saw '.'", dot);
        check("readdir() saw '..'", dotdot);
        check("readdir() saw the regular file", file_seen);
        check("readdir() reported DT_REG for the regular file", file_type == DT_REG);
        check("readdir() saw the subdirectory", sub_seen);
        check("readdir() reported DT_DIR for the subdirectory", sub_type == DT_DIR);
        check("readdir() returns NULL once exhausted", readdir(d) == NULL);
        check("closedir() succeeds", closedir(d) == 0);
    }

    DIR *missing = opendir("/no/such/directory");
    check("opendir() on a missing path returns NULL", missing == NULL);

    unlink("/dirtest_dir/afile.txt");
    rmdir("/dirtest_dir/sub");
    rmdir("/dirtest_dir");

    printf("\n%d/%d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
