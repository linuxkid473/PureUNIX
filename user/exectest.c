/*
 * user/exectest.c — regression test for real argv/envp passing through
 * fork()+execve() (kernel/elf.c's build_argv_stack()/elf_exec_current(),
 * arch/i386/syscall.c's SYS_EXEC handler, user/newlib_syscalls.c's
 * execve()). Before this, SYS_EXEC took only a bare path — no argv, no
 * envp — so a re-exec'd program could never see arguments or environment
 * variables its parent gave it.
 *
 * This program re-execs itself with a distinct argv/envp and writes what
 * the re-exec'd copy actually received to a file, since there's no pipe()
 * yet to capture a child's stdout directly (see docs on that gap). The
 * parent then reads the file back and checks it against what it asked for.
 *
 * Same harness convention as systest.c/libctest.c: numbered checks, a
 * failure never stops the run, summary at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESULT_PATH "/exectest_result.txt"

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

/* Runs as the re-exec'd child: dumps argc/argv/getenv() results to
 * RESULT_PATH so the parent can verify them, then exits with a marker code
 * distinct from any error path (so the parent can tell "ran and wrote its
 * results" from "crashed before writing anything"). */
static void run_as_child(int argc, char *argv[])
{
    FILE *f = fopen(RESULT_PATH, "w");
    if (!f) {
        exit(1);
    }
    fprintf(f, "argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        fprintf(f, "argv[%d]=%s\n", i, argv[i]);
    }
    fprintf(f, "FOO=%s\n", getenv("FOO") ? getenv("FOO") : "(null)");
    fprintf(f, "BAZ=%s\n", getenv("BAZ") ? getenv("BAZ") : "(null)");
    fprintf(f, "UNSET=%s\n", getenv("UNSET_VAR") ? getenv("UNSET_VAR") : "(null)");
    fclose(f);
    exit(42);
}

/* Same idea, but for the argv==NULL/envp==NULL ("bare pu_exec()-style
 * exec") path: dumps argc and whether any env var is visible at all. */
static void run_as_bare_child(void)
{
    FILE *f = fopen(RESULT_PATH, "w");
    if (!f) {
        exit(1);
    }
    fprintf(f, "bare_foo=%s\n", getenv("FOO") ? getenv("FOO") : "(null)");
    fclose(f);
    exit(43);
}

static int read_result(char *buf, size_t cap)
{
    FILE *f = fopen(RESULT_PATH, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        run_as_child(argc, argv);
        /* unreachable: run_as_child() always exit()s */
    }
    if (argc >= 2 && strcmp(argv[1], "--bare-child") == 0) {
        run_as_bare_child();
    }

    printf("exectest: parent starting (argc=%d)\n", argc);

    /* --- Test 1: fork() + execve(argv, envp) with real arguments --- */
    {
        pid_t pid = fork();
        if (pid < 0) {
            check("fork() succeeds", 0);
            return 1;
        }
        if (pid == 0) {
            /* Both parent and child return from fork() right here — only
             * the parent should keep running check()s past this branch, or
             * every subsequent check prints twice (once from each process,
             * sharing the same console). */
            char *const child_argv[] = { "/bin/exectest.elf", "--child", "second-arg", NULL };
            char *const child_envp[] = { "FOO=bar", "BAZ=qux", NULL };
            execve("/bin/exectest.elf", child_argv, child_envp);
            /* Only reached on failure. */
            _exit(99);
        }

        check("fork() succeeds", 1);
        int status = 0;
        pid_t reaped = waitpid(pid, &status, 0);
        check("waitpid() reaps the forked child", reaped == pid);
        check("child exited with the --child marker code (42)",
              WIFEXITED(status) && WEXITSTATUS(status) == 42);

        char buf[512];
        int rr = read_result(buf, sizeof(buf));
        check("child's result file was written", rr == 0);
        if (rr == 0) {
            check("child saw argc == 3", strstr(buf, "argc=3\n") != NULL);
            check("child saw argv[0] == /bin/exectest.elf", strstr(buf, "argv[0]=/bin/exectest.elf\n") != NULL);
            check("child saw argv[1] == --child", strstr(buf, "argv[1]=--child\n") != NULL);
            check("child saw argv[2] == second-arg", strstr(buf, "argv[2]=second-arg\n") != NULL);
            check("child saw FOO=bar via getenv()", strstr(buf, "FOO=bar\n") != NULL);
            check("child saw BAZ=qux via getenv()", strstr(buf, "BAZ=qux\n") != NULL);
            check("child saw an unset var as (null)", strstr(buf, "UNSET=(null)\n") != NULL);
        }
    }

    /* --- Test 2: fork() + execve(path, NULL, NULL) — the bare/back-compat
     * path (equivalent to pu_exec()/old execve() semantics): argv should
     * collapse to {path, NULL} and environment should be empty even though
     * this same process just had FOO set moments ago in its own env — a
     * NULL envp really does mean "empty", not "inherit". */
    {
        pid_t pid = fork();
        if (pid < 0) {
            check("fork() succeeds (test 2)", 0);
            return 1;
        }
        if (pid == 0) {
            setenv("FOO", "should-not-be-inherited", 1);
            char *const child_argv[] = { "/bin/exectest.elf", "--bare-child", NULL };
            execve("/bin/exectest.elf", child_argv, NULL);
            _exit(99);
        }

        check("fork() succeeds (test 2)", 1);
        int status = 0;
        pid_t reaped = waitpid(pid, &status, 0);
        check("waitpid() reaps the forked child (test 2)", reaped == pid);
        check("bare-envp child exited with marker code 43",
              WIFEXITED(status) && WEXITSTATUS(status) == 43);

        char buf[512];
        int rr = read_result(buf, sizeof(buf));
        check("bare-envp child's result file was written", rr == 0);
        if (rr == 0) {
            check("bare-envp child saw an empty environment (FOO not inherited)",
                  strstr(buf, "bare_foo=(null)\n") != NULL);
        }
    }

    unlink(RESULT_PATH);

    printf("\n%d/%d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
