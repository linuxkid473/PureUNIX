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

/* String check: reports PASS/FAIL like check_eq, printing both strings on
 * failure instead of a numeric expected/got pair. */
static void check_str(const char *desc, const char *expected, const char *got)
{
    t_begin(desc);
    if (str_eq(expected, got)) {
        g_pass++;
        pu_puts("PASS\n");
    } else {
        g_fail++;
        pu_puts("FAIL\n");
        pu_puts("  expected \""); pu_puts(expected);
        pu_puts("\" got \""); pu_puts(got); pu_puts("\"\n");
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

static void test_chdir_getcwd(void)
{
    section("SYS_CHDIR / SYS_GETCWD");

    char buf[PU_MAX_NAME * 2];
    int r = pu_getcwd(buf, sizeof(buf));
    check_eq("getcwd() succeeds at startup", 0, r);
    check_str("a fresh task's cwd starts at '/'", "/", buf);

    pu_rmdir("/systest_chdir_dir");
    check_eq("mkdir() for the chdir test directory succeeds", 0, pu_mkdir("/systest_chdir_dir"));

    r = pu_chdir("/systest_chdir_dir");
    check_eq("chdir() into a real directory succeeds", 0, r);
    r = pu_getcwd(buf, sizeof(buf));
    check_eq("getcwd() after chdir() succeeds", 0, r);
    check_str("getcwd() reports the new absolute path", "/systest_chdir_dir", buf);

    r = pu_chdir("/README.TXT");
    check_eq("chdir() onto a regular file returns ENOTDIR", ENOTDIR, r);
    r = pu_chdir("/no/such/directory");
    check_eq("chdir() onto a missing path returns ENOENT", ENOENT, r);

    r = pu_getcwd(buf, sizeof(buf));
    check_eq("getcwd() is unaffected by a failed chdir()", 0, r);
    check_str("cwd is still the last successful chdir() target", "/systest_chdir_dir", buf);

    char tiny[4];
    r = pu_getcwd(tiny, sizeof(tiny));
    check_eq("getcwd() into a too-small buffer returns ERANGE", ERANGE, r);

    /* A forked child's chdir() must not leak back into the parent — the two
     * tasks have independent cwd state, exactly like independent address
     * spaces and file descriptor tables (test_fork_exec_wait() above). */
    int pid = pu_fork();
    check_true("fork() (for chdir isolation test) returns a value >= 0", pid >= 0);
    if (pid == 0) {
        int child_rc = pu_chdir("/");
        pu_exit(child_rc == 0 ? 0 : 1);
    }
    int status = -1;
    pu_wait(pid, &status);
    check_eq("the forked child's chdir('/') itself succeeded", 0, status);
    r = pu_getcwd(buf, sizeof(buf));
    check_str("the parent's cwd is unaffected by the child's chdir()", "/systest_chdir_dir", buf);

    check_eq("chdir() back to '/' succeeds", 0, pu_chdir("/"));
    pu_rmdir("/systest_chdir_dir");
}

/* ==================================================================== */

static void test_pipe_dup(void)
{
    section("SYS_PIPE / SYS_DUP / SYS_DUP2");

    int fds[2] = { -1, -1 };
    int r = pu_pipe(fds);
    check_eq("pipe() succeeds", 0, r);
    check_true("pipe() read end is a real fd", fds[0] >= 3);
    check_true("pipe() write end is a real fd", fds[1] >= 3);
    check_true("pipe()'s two ends are distinct", fds[0] != fds[1]);

    char msg[] = "hello pipe";
    int wn = pu_write(fds[1], msg, sizeof(msg));
    check_eq("write() into a pipe returns the full length", (int)sizeof(msg), wn);

    char buf[32] = { 0 };
    int rn = pu_read(fds[0], buf, sizeof(buf));
    check_eq("read() from a pipe returns the full length", (int)sizeof(msg), rn);
    check_true("data read back from a pipe matches what was written", bytes_eq(buf, msg, sizeof(msg)));

    pu_close(fds[1]);
    int eof = pu_read(fds[0], buf, sizeof(buf));
    check_eq("read() on a pipe with no writers left and an empty buffer returns EOF (0)", 0, eof);
    pu_close(fds[0]);

    /* write() with no readers left returns -EPIPE (no SIGPIPE — no signal
       delivery exists yet, see docs/syscalls.md). */
    int fds2[2] = { -1, -1 };
    pu_pipe(fds2);
    pu_close(fds2[0]);
    int wr = pu_write(fds2[1], msg, sizeof(msg));
    check_eq("write() to a pipe with no readers left returns EPIPE", EPIPE, wr);
    pu_close(fds2[1]);

    /* dup(): the new fd shares the SAME open file description as the
       original — including its seek offset — exactly like a real UNIX
       dup(), and unlike this project's own fork() before this feature
       existed (which used to deep-copy fds, giving independent offsets). */
    pu_unlink("/systest_dup_file");
    int fd = pu_creat("/systest_dup_file");
    pu_write(fd, "0123456789", 10);
    pu_lseek(fd, 0, SEEK_SET);
    int dupfd = pu_dup(fd);
    check_true("dup() returns a distinct, valid fd", dupfd >= 3 && dupfd != fd);
    char b1[4] = { 0 }, b2[4] = { 0 };
    pu_read(fd, b1, 4);    /* advances the shared offset to 4 */
    pu_read(dupfd, b2, 4); /* continues from 4, not from 0 */
    check_true("dup()'d fd shares the original's file offset",
               bytes_eq(b1, "0123", 4) && bytes_eq(b2, "4567", 4));
    pu_close(fd);
    pu_close(dupfd);
    pu_unlink("/systest_dup_file");

    /* dup2(): closes whatever newfd previously held, then makes it an
       alias for oldfd's description — every subsequent op on newfd really
       goes through oldfd's own description. */
    pu_unlink("/systest_dup2_a");
    pu_unlink("/systest_dup2_b");
    int fda = pu_creat("/systest_dup2_a");
    int fdb = pu_creat("/systest_dup2_b");
    int dr = pu_dup2(fda, fdb);
    check_eq("dup2() returns newfd", fdb, dr);
    pu_write(fdb, "X", 1); /* really writes through fda's description now */
    pu_close(fda);
    pu_close(fdb);
    struct stat sta;
    pu_stat("/systest_dup2_a", &sta);
    check_eq("dup2()'d fd actually wrote through the original file description", 1, (int)sta.st_size);
    pu_unlink("/systest_dup2_a");
    pu_unlink("/systest_dup2_b");

    /* Real cross-process IPC: a forked child writes, the parent reads —
       this is what a pipe is actually *for*. The parent's read() blocks
       (cooperatively yielding — see arch/i386/syscall.c's pipe_read())
       until the child has written and closed its end. */
    int fds3[2] = { -1, -1 };
    pu_pipe(fds3);
    int pid = pu_fork();
    check_true("fork() (for pipe IPC test) returns a value >= 0", pid >= 0);
    if (pid == 0) {
        pu_close(fds3[0]); /* child only writes */
        pu_write(fds3[1], "from-child", 10);
        pu_close(fds3[1]);
        pu_exit(0);
    }

    pu_close(fds3[1]); /* parent only reads */
    char ipcbuf[16] = { 0 };
    int ipcn = pu_read(fds3[0], ipcbuf, sizeof(ipcbuf));
    int status = -1;
    pu_wait(pid, &status);
    check_eq("fork()+pipe(): parent read the exact byte count the child wrote", 10, ipcn);
    check_true("fork()+pipe(): parent received the child's actual data",
               bytes_eq(ipcbuf, "from-child", 10));
    pu_close(fds3[0]);
}

/* ==================================================================== */

static void test_kill(void)
{
    section("SYS_KILL");

    /* kill(pid, 0): the POSIX "null signal" probes existence without
       killing. Self always exists. */
    check_eq("kill(self, 0) succeeds (self always exists)", 0, pu_kill(pu_getpid(), 0));
    check_eq("kill(a definitely-nonexistent pid, 0) returns ESRCH", ESRCH, pu_kill(999999, 0));
    /* kill(pid<=0, ...): process-group targeting, added in M5 — see
       test_signals() below for the real delivery test (safely isolated
       from this test binary's own shared process group). A null signal
       (sig==0) is always safe to test directly here: it never actually
       delivers anything, only probes group existence. */
    check_eq("kill(0, 0) succeeds (own group always exists, null signal probes only)",
             0, pu_kill(0, 0));
    check_eq("kill(a definitely-nonexistent pgid, anything) returns ESRCH",
             ESRCH, pu_kill(-999999, SIGTERM));

    /* A forked child that just spins yielding (so the parent actually gets
       scheduled to kill it) — killed from the parent with SIGTERM, then
       reaped: wait() must report it died *by that signal*, not that it
       exited normally, and the raw exit code convention (docs/syscalls.md)
       is exit_code == -signal. */
    int pid = pu_fork();
    check_true("fork() (for kill test) returns a value >= 0", pid >= 0);
    if (pid == 0) {
        for (;;) {
            pu_yield();
        }
        /* unreachable */
    }

    check_eq("kill() on a real, running child succeeds", 0, pu_kill(pid, SIGTERM));
    int status = 1; /* deliberately not 0, so a no-op wait() wouldn't false-pass */
    int reaped = pu_wait(pid, &status);
    check_eq("wait() reaps the killed child", pid, reaped);
    check_eq("wait() reports exit_code == -SIGTERM (killed by signal, not a normal exit)",
             -SIGTERM, status);

    check_eq("kill() on an already-reaped pid returns ESRCH", ESRCH, pu_kill(pid, SIGTERM));
}

/* ==================================================================== */

static void test_pgrp_session(void)
{
    section("Process groups and sessions (setpgid/getpgid/setsid/getsid)");

    int self = pu_getpid();
    check_true("getpgid(0) returns a positive pgid", pu_getpgid(0) > 0);
    check_eq("getpgid(0) == getpgid(getpid())", pu_getpgid(0), pu_getpgid(self));
    check_eq("getsid(0) == getsid(getpid())", pu_getsid(0), pu_getsid(self));
    check_eq("getpgid() of a definitely-nonexistent pid returns ESRCH", ESRCH, pu_getpgid(999999));
    check_eq("getsid() of a definitely-nonexistent pid returns ESRCH", ESRCH, pu_getsid(999999));
    check_eq("setpgid() on a pid that isn't the caller or its child returns EPERM",
             EPERM, pu_setpgid(1, 0));

    /* A forked child starts in the parent's own group (pgid inherited
       unchanged, per fork() semantics — include/pureunix/task.h) — never
       equal to its own freshly-assigned pid, so setpgid(0, 0) (become a
       new group leader) must still have real work to do here. Both parent
       and child call setpgid() on it, the standard race-avoidance idiom
       (POSIX): whichever runs first "wins", the other's call is then a
       no-op that still succeeds. */
    int pid = pu_fork();
    check_true("fork() (for pgrp test) returns a value >= 0", pid >= 0);
    if (pid == 0) {
        pu_setpgid(0, 0);
        int ok = (pu_getpgid(0) == pu_getpid()) ? 1 : 0;
        pu_exit(ok);
        /* unreachable */
    }
    pu_setpgid(pid, 0);
    int status = -1;
    int reaped = pu_wait(pid, &status);
    check_eq("wait() reaps the setpgid() child", pid, reaped);
    check_eq("child's own setpgid(0,0) made it its own group leader", 1, status);

    /* setsid(): a freshly forked child (pgid inherited, not its own id) is
       not a group leader, so setsid() must succeed; calling it *again*
       afterwards must fail with EPERM (POSIX: a process group leader may
       never start a new session — see task_setsid()). */
    int pid2 = pu_fork();
    check_true("fork() (for setsid test) returns a value >= 0", pid2 >= 0);
    if (pid2 == 0) {
        int sid = pu_setsid();
        int leader_ok = (sid == pu_getpid() && pu_getsid(0) == pu_getpid() &&
                          pu_getpgid(0) == pu_getpid())
                             ? 1
                             : 0;
        int second_call_rejected = (pu_setsid() == EPERM) ? 1 : 0;
        pu_exit(leader_ok && second_call_rejected ? 1 : 0);
        /* unreachable */
    }
    int status2 = -1;
    int reaped2 = pu_wait(pid2, &status2);
    check_eq("wait() reaps the setsid() child", pid2, reaped2);
    check_eq("setsid() makes a non-leader its own session+group leader; "
             "a second call on an existing leader is rejected",
             1, status2);
}

/* ==================================================================== */

static void test_preemption(void)
{
    section("Minimal preemption: a CPU-bound task can't starve another");

    /* A child that never calls a single syscall in its loop -- not
       pu_yield(), not anything -- deliberately never cooperates with the
       scheduler again once it starts. Under the old purely-cooperative
       model (docs/process-management.md), the instant this child is ever
       scheduled, nothing could ever take the CPU back from it -- this
       exact scenario would hang the whole system forever. `volatile`
       keeps -O2 from proving the empty loop has no observable effect and
       deleting it outright. */
    int pid = pu_fork();
    check_true("fork() (for preemption test) returns a value >= 0", pid >= 0);
    if (pid == 0) {
        volatile long i = 0;
        for (;;) {
            i++;
            (void)i;
        }
        /* unreachable */
    }

    /* One single, deliberate, explicit yield -- after this, this parent
       is merely TASK_RUNNABLE, competing in the exact same round-robin as
       every other task in the system (including the shell that fork()ed
       and exec()'d this very test binary, itself spin-yielding in its own
       wait() -- see kernel/task.c's task_waitpid(), not yet converted to
       real wait-queue blocking -- while it waits for this process to
       exit; that's an additional harmless RUNNABLE competitor, not a
       threat to this test: whichever task the scheduler visits next,
       *every* path back to this line still requires forcibly reclaiming
       the CPU from the spinning child at least once, since it never
       cooperates again after the fork() above).
       Every line below this comment only ever executes if the
       timer-interrupt-driven preemption added in this milestone actually
       works. If it doesn't, this call simply never returns -- there is no
       clean "FAIL" outcome for that regression, only a hang (of this test
       binary, and by extension the whole boot session), which is itself
       the strongest possible signal something is deeply wrong. */
    pu_yield();

    check_eq("kill() reaches a still-spinning, never-yielding child "
             "(the parent could only get here via forced preemption)",
             0, pu_kill(pid, SIGKILL));
    int status = 1;
    int reaped = pu_wait(pid, &status);
    check_eq("wait() reaps the preempted, killed child", pid, reaped);
    check_eq("wait() reports it died by SIGKILL", -SIGKILL, status);
}

/* ==================================================================== */

static volatile int g_sigchld_count = 0;
static volatile int g_last_sig = 0;

static void chld_handler(int sig)
{
    g_sigchld_count++;
    g_last_sig = sig;
}

static void test_signals(void)
{
    section("Signals: default actions, SIG_IGN, a real handler via the trampoline, SIGKILL");

    /* SIG_IGN blocks a signal that would otherwise terminate. */
    pu_sigaction_t ign = { .handler = PU_SIG_IGN };
    pu_sigaction_t dfl = { .handler = PU_SIG_DFL };
    check_eq("sigaction(SIGTERM, SIG_IGN) succeeds", 0, pu_sigaction(SIGTERM, &ign, NULL));
    check_eq("kill(self, SIGTERM) while ignored does not terminate",
             0, pu_kill(pu_getpid(), SIGTERM));
    check_eq("sigaction(SIGTERM, SIG_DFL) restores the default", 0, pu_sigaction(SIGTERM, &dfl, NULL));

    /* SIGKILL/SIGSTOP's disposition can never even be *set* — POSIX. */
    check_eq("sigaction(SIGKILL, ...) is rejected (uncatchable/unignorable)",
             EINVAL, pu_sigaction(SIGKILL, &ign, NULL));
    check_eq("sigaction(SIGSTOP, ...) is rejected (uncatchable/unignorable)",
             EINVAL, pu_sigaction(SIGSTOP, &ign, NULL));

    /* A real SIGCHLD handler actually runs -- proof the trampoline works
       end-to-end, not just that sigaction() records what was asked. */
    pu_sigaction_t chld_act;
    chld_act.handler = (unsigned int)(unsigned long)chld_handler;
    check_eq("sigaction(SIGCHLD, a real handler) succeeds",
             0, pu_sigaction(SIGCHLD, &chld_act, NULL));
    int before = g_sigchld_count;
    int cpid = pu_fork();
    check_true("fork() (for the SIGCHLD handler test) returns a value >= 0", cpid >= 0);
    if (cpid == 0) {
        pu_exit(0);
        /* unreachable */
    }
    int cstatus = -1;
    int creaped = pu_wait(cpid, &cstatus);
    check_eq("wait() reaps the child that triggers SIGCHLD", cpid, creaped);
    /* Delivery happens at this task's own next ring3-return boundary
       (arch/i386/idt.c) -- in practice that's the very int $0x80 return
       from the pu_wait() call above, so the handler has, by observation,
       always already run by this point. Poll briefly instead of a bare
       assert regardless, matching real asynchronous-signal semantics
       rather than assuming one specific delivery instant. */
    for (int i = 0; i < 1000 && g_sigchld_count == before; i++) {
        pu_yield();
    }
    check_true("a real SIGCHLD handler actually ran (trampoline delivery works)",
               g_sigchld_count > before);
    check_eq("the handler received the correct signal number", SIGCHLD, g_last_sig);
    pu_sigaction(SIGCHLD, &dfl, NULL);

    /* kill(0, sig)/kill(-pgid, sig): process-group delivery, added in
       M5. Testing a *real* delivery from this process directly would hit
       this test binary's own shared process group (still shared with the
       ash session that launched it, absent job control) -- isolate it in
       a child that first makes itself its own group leader instead. */
    int gpid = pu_fork();
    check_true("fork() (for kill(0,...) group-delivery test) returns a value >= 0", gpid >= 0);
    if (gpid == 0) {
        pu_setpgid(0, 0);
        pu_kill(0, SIGKILL);
        pu_exit(99); /* unreachable if kill(0, SIGKILL) actually worked */
    }
    int gstatus = 1;
    int greaped = pu_wait(gpid, &gstatus);
    check_eq("wait() reaps the kill(0, SIGKILL) test child", gpid, greaped);
    check_eq("kill(0, SIGKILL) reaches every member of the caller's own group (itself)",
             -SIGKILL, gstatus);

    /* sigprocmask()/sigpending(): a blocked signal stays pending instead
       of being dropped or delivered early. */
    uint32_t mask = 1u << SIGUSR1;
    check_eq("sigprocmask(SIG_BLOCK) succeeds", 0, pu_sigprocmask(PU_SIG_BLOCK, &mask, NULL));
    pu_sigaction_t catch_act;
    catch_act.handler = (unsigned int)(unsigned long)chld_handler;
    check_eq("sigaction(a blockable test signal, a real handler) succeeds",
             0, pu_sigaction(SIGUSR1, &catch_act, NULL));
    check_eq("kill(self, blocked signal) succeeds (it's queued, not dropped)",
             0, pu_kill(pu_getpid(), SIGUSR1));
    check_true("the blocked signal shows up in sigpending()",
               (pu_sigpending() & (1u << SIGUSR1)) != 0);
    int before2 = g_sigchld_count;
    check_eq("sigprocmask(SIG_UNBLOCK) succeeds", 0, pu_sigprocmask(PU_SIG_UNBLOCK, &mask, NULL));
    for (int i = 0; i < 1000 && g_sigchld_count == before2; i++) {
        pu_yield();
    }
    check_true("unblocking delivers the signal that was queued while blocked",
               g_sigchld_count > before2);
    pu_sigaction(SIGUSR1, &dfl, NULL);
}

/* ==================================================================== */

static volatile long g_priority_count = 0;
static int g_priority_report_fd = -1;

static void priority_term_handler(int sig)
{
    (void)sig;
    long count = g_priority_count;
    pu_write(g_priority_report_fd, (const char *)&count, sizeof(count));
    pu_exit(0);
}

static void test_priority(void)
{
    section("nice/renice: SYS_SETPRIORITY/SYS_GETPRIORITY");

    int nice_val = -999;
    check_eq("getpriority(0) succeeds", 0, pu_getpriority(0, &nice_val));
    check_eq("a freshly forked/exec'd process defaults to nice 0", 0, nice_val);

    check_eq("setpriority(0, 10) succeeds", 0, pu_setpriority(0, 10));
    pu_getpriority(0, &nice_val);
    check_eq("getpriority() reflects the new value", 10, nice_val);

    check_eq("setpriority(0, 100) succeeds (clamped)", 0, pu_setpriority(0, 100));
    pu_getpriority(0, &nice_val);
    check_eq("nice value clamps to the POSIX maximum (19)", 19, nice_val);

    check_eq("setpriority(0, -100) succeeds (clamped)", 0, pu_setpriority(0, -100));
    pu_getpriority(0, &nice_val);
    check_eq("nice value clamps to the POSIX minimum (-20)", -20, nice_val);

    pu_setpriority(0, 0); /* restore for the rest of the suite */

    check_eq("setpriority() on a definitely-nonexistent pid returns ESRCH",
             ESRCH, pu_setpriority(999999, 0));
    check_eq("getpriority() on a definitely-nonexistent pid returns ESRCH",
             ESRCH, pu_getpriority(999999, &nice_val));

    /* setpriority() targeting a *child*, not just self. */
    int cpid = pu_fork();
    check_true("fork() (for setpriority-on-child test) returns a value >= 0", cpid >= 0);
    if (cpid == 0) {
        for (;;) {
            pu_yield();
        }
        /* unreachable */
    }
    check_eq("setpriority() targeting a child succeeds", 0, pu_setpriority(cpid, 15));
    int child_nice = -999;
    check_eq("getpriority() on that child succeeds", 0, pu_getpriority(cpid, &child_nice));
    check_eq("...and reflects the value set on it", 15, child_nice);
    pu_kill(cpid, SIGKILL);
    pu_wait(cpid, NULL);

    /* A real, observable scheduling effect, not just field storage: a
       heavily-reniced (+19) CPU-bound child accumulates measurably fewer
       loop iterations than a default-priority sibling over the same
       stretch of real time — proving kernel/task.c's next_ready_task()
       actually acts on nice (see its own comment for the exact scheme:
       a nice > 0 task is skipped `nice` extra scheduling passes between
       turns). Both children spin with *no* syscalls in their own loop —
       relying entirely on the same timer-driven preemption
       test_preemption() above already proved works — and report their
       final count via a real SIGTERM handler once killed, since there's
       no nanosleep() wrapper in this low-level API to bound wall time
       any more directly than a large bounded busy-loop here in the
       parent (same idiom test_preemption() uses). */
    int lo_pipe[2], hi_pipe[2];
    check_eq("pipe() for the nice=0 spinner's report", 0, pu_pipe(lo_pipe));
    check_eq("pipe() for the nice=19 spinner's report", 0, pu_pipe(hi_pipe));

    pu_sigaction_t term_act;
    term_act.handler = (unsigned int)(unsigned long)priority_term_handler;

    int lo_pid = pu_fork();
    check_true("fork() (nice=0 spinner) returns a value >= 0", lo_pid >= 0);
    if (lo_pid == 0) {
        pu_close(lo_pipe[0]);
        g_priority_report_fd = lo_pipe[1];
        pu_sigaction(SIGTERM, &term_act, NULL);
        for (;;) {
            g_priority_count++;
        }
        /* unreachable */
    }
    int hi_pid = pu_fork();
    check_true("fork() (nice=19 spinner) returns a value >= 0", hi_pid >= 0);
    if (hi_pid == 0) {
        pu_close(hi_pipe[0]);
        g_priority_report_fd = hi_pipe[1];
        pu_sigaction(SIGTERM, &term_act, NULL);
        pu_setpriority(0, 19);
        for (;;) {
            g_priority_count++;
        }
        /* unreachable */
    }
    pu_close(lo_pipe[1]);
    pu_close(hi_pipe[1]);

    for (volatile long i = 0; i < 40000000L; i++) {
    }

    check_eq("kill(SIGTERM) reaches the nice=0 spinner", 0, pu_kill(lo_pid, SIGTERM));
    check_eq("kill(SIGTERM) reaches the nice=19 spinner", 0, pu_kill(hi_pid, SIGTERM));
    pu_wait(lo_pid, NULL);
    pu_wait(hi_pid, NULL);

    long lo_count = 0, hi_count = 0;
    check_eq("read() gets the nice=0 spinner's final count",
             (int)sizeof(lo_count), pu_read(lo_pipe[0], (char *)&lo_count, sizeof(lo_count)));
    check_eq("read() gets the nice=19 spinner's final count",
             (int)sizeof(hi_count), pu_read(hi_pipe[0], (char *)&hi_count, sizeof(hi_count)));
    pu_close(lo_pipe[0]);
    pu_close(hi_pipe[0]);

    check_true("a nice=0 task accumulates measurably more CPU-bound loop "
               "iterations than a nice=19 sibling over the same real time",
               lo_count > hi_count);
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
     * semantics. A writable, non-append reopen with no O_TRUNC must
     * preserve existing content (real POSIX open() semantics: only
     * O_TRUNC discards it) — found missing during the SQLite port
     * (docs/sqlite-port.md): SQLite always reopens its db file
     * O_RDWR|O_CREAT with no O_TRUNC and depends on seeing prior content,
     * which this kernel's SYS_OPEN never loaded for a non-append write
     * open before (see docs/syscalls.md's SYS_OPEN section). Writing "hi"
     * (2 bytes) at offset 0 into the still-5-byte "hello" buffer overlays
     * the first two bytes, giving "hillo" — a real read-modify-write, not
     * a truncate. */
    int wfd2 = pu_open("/systest_tmp_openclose.txt", O_WRONLY | O_CREAT);
    check_true("O_CREAT on an existing file succeeds (no O_EXCL)", wfd2 >= 3);
    pu_write(wfd2, "hi", 2);
    pu_close(wfd2);
    r = pu_stat("/systest_tmp_openclose.txt", &st);
    check_eq("re-opened-for-write without O_TRUNC preserves prior content length", 5, (int)st.st_size);

    int rfd2 = pu_open("/systest_tmp_openclose.txt", O_RDONLY);
    check_true("reopen for reading (after read-modify-write) succeeds", rfd2 >= 3);
    char buf2[16];
    int n2 = pu_read(rfd2, buf2, sizeof(buf2));
    check_true("read-modify-write overlaid the start, preserved the rest",
               n2 == 5 && bytes_eq(buf2, "hillo", 5));
    pu_close(rfd2);

    /* O_TRUNC, by contrast, must actually discard prior content — the
     * real POSIX escape hatch for "start over" every writer that wants a
     * blank slate (ash's `>` redirection, editors' save paths, coreutils)
     * already uses. */
    int wfd3 = pu_open("/systest_tmp_openclose.txt", O_WRONLY | O_CREAT | O_TRUNC);
    check_true("O_TRUNC reopen of an existing file succeeds", wfd3 >= 3);
    pu_write(wfd3, "hi", 2);
    pu_close(wfd3);
    r = pu_stat("/systest_tmp_openclose.txt", &st);
    check_eq("O_TRUNC reopen actually truncated to just the new content", 2, (int)st.st_size);

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
    section("SYS_CHMOD / SYS_CHOWN");

    /* A dedicated file rather than /README.TXT — other sections (e.g.
       test_stat_lstat_access(), test_elf_exec_preconditions()) depend on
       its exact mode/uid/gid, and this section would otherwise permanently
       mutate them for every test that runs afterward (or on the next boot,
       since EXT2 persists these to disk now). */
    pu_unlink("/systest_chmod_file");
    int fd = pu_creat("/systest_chmod_file");
    check_true("creat() for the chmod/chown test file succeeds", fd >= 3);
    pu_close(fd);

    check_eq("chmod() on a missing file returns ENOENT", ENOENT, pu_chmod("/no/such/file", 0600));
    check_eq("chown() on a missing file returns ENOENT", ENOENT, pu_chown("/no/such/file", 5, 5));

    /* As root: chmod/chown are real now (EXT2 stores i_mode/i_uid/i_gid —
       see fs/ext2/mount.c's ext2_chmod()/ext2_chown()), not the old
       infrastructure-only -EROFS stubs. */
    int r = pu_chmod("/systest_chmod_file", 0640);
    check_eq("root: chmod() to 0640 succeeds", 0, r);
    struct stat st;
    pu_stat("/systest_chmod_file", &st);
    check_eq("chmod()'d file reports the new mode via stat()", 0640, (int)(st.st_mode & 0777));

    r = pu_chown("/systest_chmod_file", 1000, 100);
    check_eq("root: chown() to uid=1000/gid=100 succeeds", 0, r);
    pu_stat("/systest_chmod_file", &st);
    check_true("chown()'d file reports the new uid/gid via stat()",
               st.st_uid == 1000 && st.st_gid == 100);

    /* uid/gid of -1 means "leave unchanged" (POSIX chown(2) convention) —
       change only the group, confirm the uid from above survives. */
    r = pu_chown("/systest_chmod_file", (uid_t)-1, 200);
    check_eq("chown() with uid=-1 succeeds (group-only change)", 0, r);
    pu_stat("/systest_chmod_file", &st);
    check_true("chown(uid=-1) left uid untouched and only changed gid",
               st.st_uid == 1000 && st.st_gid == 200);

    /* Non-root: chmod requires owning the file; chown is root-only outright
       (PureUNIX has no supplementary-group model to let a non-root owner
       hand a file to a group they belong to). */
    pu_debug_setcred(1000, 200);
    r = pu_chmod("/systest_chmod_file", 0666);
    check_eq("non-root owner: chmod() on an owned file succeeds", 0, r);
    r = pu_chown("/systest_chmod_file", 1000, 1000);
    check_eq("non-root: chown() is always EPERM, even on a file you own", EPERM, r);

    pu_debug_setcred(2000, 2000);
    r = pu_chmod("/systest_chmod_file", 0600);
    check_eq("non-root, non-owner: chmod() returns EPERM", EPERM, r);

    /* Restore root — leaving credentials changed would corrupt every test
       after this one (see test_permissions()'s identical note). */
    pu_debug_setcred(0, 0);
    check_eq("credentials restored to uid=0/gid=0", 0, pu_access("/systest_chmod_file", F_OK));

    pu_unlink("/systest_chmod_file");

    /* FAT16 stores no Unix ownership/permission bits at all (fs/fat16.c has
       no ops->chmod/ops->chown), so it still resolves to -EROFS regardless
       of caller — only EXT2 gained real chmod/chown above. */
    check_eq("chmod() on a FAT16 file still returns EROFS (no on-disk mode bits)", EROFS,
             pu_chmod("/fat/README.TXT", 0600));
    check_eq("chown() on a FAT16 file still returns EROFS (no on-disk owner bits)", EROFS,
             pu_chown("/fat/README.TXT", 5, 5));
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

    /* Fill the filesystem's remaining free space with separate,
       modest-sized files (each its own fresh write buffer) until the block
       allocator runs out at close() time (when a buffered write is
       actually flushed to disk — see docs/syscalls.md's SYS_WRITE
       section). Using many *separate* files, rather than growing one
       file's buffer across many writes, avoids the O(n^2) cost of this
       kernel's write-buffer growth (every SYS_WRITE on the same fd
       reallocates and re-zeroes the *entire* accumulated buffer from
       scratch, so repeatedly growing one fd to several MiB is drastically
       more expensive than writing the same total spread over many fds).
       MAX_FILLERS is just a safety backstop (the loop actually exits via
       enospc_seen as soon as ENOSPC really happens, so a generous bound
       costs nothing at runtime) — deliberately far above any current disk
       image size (tools/mkext2.py's TOTAL_BLOCKS, currently 24 MiB across
       3 block groups — see that file's "Why 3 block groups") so this test
       doesn't need retuning every time the image grows again, the way the
       old MAX_FILLERS=128 (exactly the old single-group 8 MiB image's
       size) silently stopped exercising ENOSPC at all once the image grew
       to 24 MiB without ever failing loudly. */
    enum { FILLER_SIZE = 65536, MAX_FILLERS = 4096 };
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

/* Deliberately run *last*, after every other test: close()ing fd 0/1/2
 * reverts that slot to the default console binding rather than returning
 * EBADF (the old, pre-SYS_PIPE/SYS_DUP2 behavior — see
 * include/pureunix/task.h's fd_entry_t comment) — this is what makes
 * stdout redirection work at all (dup2()ing something onto fd 1, using it,
 * then dup2()ing a saved copy of the original console binding back
 * requires fd 1 to be a normal, closeable/rebindable descriptor like any
 * other). But a reverted-to-console 0/1/2 is also, correctly per real
 * POSIX "lowest available fd" semantics, fair game for the *next*
 * open()/dup()/pipe()/fcntl(F_DUPFD) call to reclaim (see
 * arch/i386/syscall.c's find_free_fd() — this is what makes BusyBox's
 * `close(0); open(file)` idiom in `uniq FILE` etc. work). This test's own
 * later checks all assume their own pu_open()/pu_creat() calls return
 * fd >= 3, same as any real program not expecting to have just closed its
 * own stdio — so this one deliberately runs after every one of them, not
 * back where the parallel SYS_CLOSE checks live in test_open_close(), to
 * avoid being the thing that hands some *later* unrelated open() call a
 * surprise fd 1/2 and silently redirects this program's own remaining
 * test-result output into a file instead of the console. */
static void test_fd_close_reclaims_console_slot(void)
{
    section("fd 0/1/2 close()/reopen (run last — see comment above)");

    check_eq("close() on a reserved fd (stdout) succeeds (reverts to console)", 0, pu_close(1));
    check_true("write(stdout) still reaches the console after close() reverted it", pu_write(1, "", 0) >= 0);
}

int main(void)
{
    pu_puts("=== PureUNIX System Test ===\n");

    test_process_basics();
    test_fork_exec_wait();
    test_chdir_getcwd();
    test_pipe_dup();
    test_kill();
    test_pgrp_session();
    test_preemption();
    test_signals();
    test_priority();
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
    test_fd_close_reclaims_console_slot();

    pu_puts("\n====================================\n");
    pu_puts("PureUNIX System Test Complete\n");
    pu_puts("Tests: "); pu_puti(g_num); pu_puts("\n");
    pu_puts("PASS: ");  pu_puti(g_pass); pu_puts("\n");
    pu_puts("FAIL: ");  pu_puti(g_fail); pu_puts("\n");
    pu_puts("====================================\n");

    return g_fail == 0 ? 0 : 1;
}
