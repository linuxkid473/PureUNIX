#ifndef PUREUNIX_TASK_H
#define PUREUNIX_TASK_H

#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/types.h>
#include <pureunix/wait.h>

#define MAX_OPEN_FILES 16

typedef enum fd_kind {
    FD_KIND_FILE = 0,
    FD_KIND_PIPE = 1,
    /* A /dev/ttyN (or /dev/tty) descriptor opened explicitly via SYS_OPEN
     * (arch/i386/syscall.c's /dev/tty interception) — distinct from fd
     * 0/1/2's *default* console binding (file == NULL, see fd_entry_t
     * below), which never allocates an open_file_t at all. */
    FD_KIND_TTY = 2,
    /* /dev/null: reads always report EOF, writes always succeed and
     * discard their data, matching the real device's semantics exactly.
     * No backing vfs_read_file()/vfs_write_file() round-trip at all —
     * see arch/i386/syscall.c's /dev/null interception in SYS_OPEN,
     * mirroring the /dev/tty interception's pattern. Needed for BusyBox
     * ash job control (M9): backgrounding a job with no explicit
     * redirection (`cmd &`) redirects its stdin to /dev/null. */
    FD_KIND_NULL = 3,
} fd_kind_t;

/* Ring buffer shared by both ends of one pipe() call — allocated once per
 * pipe() (SYS_PIPE, arch/i386/syscall.c) and freed once both ends' last
 * reference is gone. read_ends/write_ends count *distinct open_file_t's*
 * holding that end open, not fd-table slots aliasing one of them (dup()/
 * dup2()/fork() bump an open_file_t's own refcount instead — see below). */
#define PUREUNIX_PIPE_SIZE 4096
typedef struct pipe_buf {
    uint8_t data[PUREUNIX_PIPE_SIZE];
    size_t head, tail, count;
    int read_ends;
    int write_ends;
    /* Real wait queues (kernel/wait.c) a blocked reader/writer sleeps on —
     * read_wq is woken whenever count grows (or write_ends drops to 0, an
     * EOF condition readers must also wake up for); write_wq is woken
     * whenever count shrinks (or read_ends drops to 0, an EPIPE condition
     * writers must also wake up for). Replaces the old bare
     * task_yield()-spin pipe blocking. */
    wait_queue_t read_wq;
    wait_queue_t write_wq;
} pipe_buf_t;

/* An open file description (POSIX's term) — the thing dup()/dup2()/fork()
 * actually share between multiple fd-table slots, distinct from a file
 * descriptor (just a task-local index into fd_entry_t[] pointing at one of
 * these). Refcounted: open_file_unref() only actually frees it (flushing a
 * FD_KIND_FILE write to the VFS, or retiring a FD_KIND_PIPE end) once the
 * last fd slot referencing it — across every task that shared it via
 * dup()/dup2()/fork() — has been closed. */
typedef struct open_file {
    int refcount;
    fd_kind_t kind;
    int flags; /* open() flags: O_RDONLY / O_WRONLY / O_RDWR */

    /* FD_KIND_FILE: whole-file-in-memory, same model this project has
     * always used — data is the entire file loaded at open() time via
     * vfs_read_file(), flushed back in one shot via vfs_write_file() when
     * the last reference is closed. */
    char     path[PUREUNIX_MAX_PATH]; /* VFS path — used for the close()-time flush */
    uint8_t *data;                    /* kmalloc'd file contents; freed on close */
    size_t   size;                    /* total file size in bytes */
    size_t   offset;                  /* current seek position — shared by every fd
                                        * slot referencing this description, exactly
                                        * like a real UNIX open file description */

    /* FD_KIND_PIPE: which end of *pipe_buf this description represents
     * (a pipe() call always produces two open_file_t's sharing one
     * pipe_buf_t, one per end). */
    pipe_buf_t *pipe_buf;
    bool pipe_is_write_end;

    /* FD_KIND_TTY: which VT (0-based) this /dev/ttyN descriptor targets —
     * see arch/i386/syscall.c's SYS_OPEN /dev/tty interception and
     * include/pureunix/vt.h. Independent of the *opening* task's own
     * vt_id: e.g. a shell on VT1 opening /dev/tty3 gets a descriptor bound
     * to VT3 regardless of where it was opened from. */
    int tty_vt_id;
} open_file_t;

/* One slot in a task's file descriptor table. Slots 0/1/2 start out with
 * file == NULL, meaning "the default console binding" (SYS_READ/SYS_WRITE
 * special-case fd 0/1/2 with a null file pointer straight to the tty/VGA
 * driver — see arch/i386/syscall.c); dup2()ing something onto 0/1/2
 * installs a real open_file_t there instead, exactly like redirecting a
 * real UNIX process's stdin/stdout/stderr. Explicitly close()ing 0/1/2
 * reverts to the console binding rather than leaving the slot truly
 * closed (`used` stays true) — deliberately simpler than real UNIX, where
 * it would become a genuinely invalid fd.
 *
 * That still has to be told apart from a slot that's console-bound for any
 * *other* reason — never touched at all (the task-creation-time default),
 * or actively holding a deliberate console binding again after a
 * dup2()-based redirect-then-restore cycle (real shells, including
 * BusyBox ash, do exactly this around every redirected builtin: save fd 1
 * with dup()/fcntl(F_DUPFD), dup2() the redirect target onto fd 1, run the
 * command, dup2() the saved copy back onto fd 1). All three end up
 * `used == true, file == NULL` and *none* of the other two should be
 * handed out by a fresh open()/dup()/pipe()/fcntl(F_DUPFD) allocation —
 * only a slot that was actually close()'d should be. Real programs rely on
 * that specific case: BusyBox's `uniq FILE` (and several other coreutils'
 * optional-FILE-argument handling) does `close(0); open(path)` specifically
 * to make the opened file *become* fd 0, the standard POSIX "lowest
 * available fd" idiom. `closed_explicitly` is the bit that resolves the
 * ambiguity: false from task creation, set true only by an actual close()
 * of 0/1/2 (see SYS_CLOSE), and cleared back to false by *any* allocation
 * into the slot — including a dup2()-based restore, which is a deliberate,
 * exclusive claim on the slot, not an abandonment of it, even though the
 * thing being dup2()'d back in happens to be another NULL/console binding.
 * Meaningless for fd >= 3, where `used` alone already fully describes
 * availability (close() there really does clear it). */
typedef struct fd_entry {
    bool used;
    bool closed_explicitly;
    open_file_t *file;
    /* Set only by fcntl(F_DUPFD_CLOEXEC) (arch/i386/syscall.c) — every
     * other allocator (open()/dup()/dup2()/pipe()/plain F_DUPFD) leaves
     * this false. A real, honored close-on-exec model (kernel/elf.c's
     * elf_exec_argv()/elf_exec_current() close every cloexec fd before
     * installing the new program image) — not a no-op like it used to be.
     * BusyBox ash's setjobctl() (job control, M9) relies on this: it
     * F_DUPFD_CLOEXECs its controlling-terminal fd specifically so it
     * doesn't leak into every child it forks+execs, silently stealing one
     * fd-table slot from each of them otherwise. */
    bool cloexec;
} fd_entry_t;

/* Allocates a new, zeroed, refcount-1 open file description of the given
 * kind. Returns NULL on allocation failure. */
open_file_t *open_file_alloc(fd_kind_t kind);
/* Bumps refcount — used by dup()/dup2() (arch/i386/syscall.c) and by
 * task_fork() below (fork() shares open file descriptions with the
 * child, per POSIX — including the current seek offset — rather than
 * deep-copying them, exactly like a real UNIX fork()). */
void open_file_ref(open_file_t *f);
/* Drops refcount; once it reaches zero, flushes a FD_KIND_FILE's buffered
 * writes to the VFS (vfs_write_file()) or retires a FD_KIND_PIPE end
 * (decrementing pipe_buf's read_ends/write_ends, freeing pipe_buf itself
 * once both are zero), then frees f. Safe to call with f == NULL (no-op) —
 * every fd_entry_t's file starts NULL for the console-bound stdio slots.
 * Returns the flush's result (0 or a negative errno) if this was the last
 * reference to a written FD_KIND_FILE, or 0 otherwise — SYS_CLOSE
 * (arch/i386/syscall.c) surfaces this as its own return value; dup2()/
 * fork()'s cleanup paths ignore it, matching real close()/dup2() (which
 * never reports another fd's flush errors either). */
int open_file_unref(open_file_t *f);

typedef enum task_state {
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_SLEEPING,
    /* Job-control-stopped (SIGTSTP/SIGSTOP-equivalent — see
     * docs/process-management.md). Never selected by next_ready_task()
     * (kernel/task.c), same as TASK_SLEEPING/TASK_ZOMBIE; resumed back to
     * TASK_RUNNABLE only by an explicit SIGCONT (kernel/signal.c, M5). */
    TASK_STOPPED,
    TASK_ZOMBIE,
} task_state_t;

typedef struct task {
    uint32_t id;
    char name[32];
    task_state_t state;
    uint32_t *stack_ptr;
    uint8_t *stack_base;
    void (*entry)(void *);
    void *arg;
    /* set for tasks started via task_create_user(): task_bootstrap() drops
     * to ring 3 at user_entry/user_stack instead of calling entry(arg). */
    bool is_user;
    uint32_t user_entry;
    uint32_t user_stack;
    /* physical address of this task's page directory (CR3 value). Kernel-
     * mode tasks and the initial "kernel" task all share
     * vmm_kernel_directory_phys(); tasks created via task_create_user()/
     * task_fork() get their own private one. */
    uint32_t pd_phys;
    /* Creator's pid, used by task_waitpid()/task_exit() to find a caller's
     * own children — a scalar id rather than a `struct task *`, because a
     * raw pointer would dangle the instant the parent itself exits and gets
     * reaped (kfree'd) while orphaned children still point at it; task_id
     * 0 never exists (ids start at 1), so it doubles as "no parent" for the
     * boot task. Re-parented to g_init_pid by task_exit() when this task's
     * own parent exits first — see docs/process-management.md. */
    uint32_t ppid;
    /* Process group id and session id (POSIX setpgid()/setsid()). A fresh
     * task inherits both unchanged from its creator (task_alloc()); the
     * session-leader task of each VT sets sid == pgid == its own id at
     * startup (kernel/main.c). */
    uint32_t pgid;
    uint32_t sid;
    /* NUL-separated argv as passed to exec()/fork() — e.g. "ls\0-la\0"
     * for `ls -la` — used by ps/top (via /proc/[pid]/cmdline) and by
     * task_alloc()'s default (just the task's name, NUL-terminated) before
     * a real exec() overwrites it. Always NUL-terminated even when
     * truncated. */
    char cmdline[128];
    /* time_now() (kernel/time.c) at task creation — Unix epoch seconds. */
    uint32_t start_time;
    /* Real total bytes mapped into this task's address space at the last
     * exec() — every PT_LOAD segment's page-aligned size (kernel/elf.c's
     * elf_load_into()) plus the fixed USER_STACK_SIZE stack region, not a
     * fixed per-task constant. 0 for a kernel-only task (never execs a
     * user ELF). Genuinely varies per program (e.g. a tiny "hello" vs.
     * ncdemo's ~1.3 MiB of ncurses), unlike the coarse fixed-window
     * estimate this used to be — see fs/procfs.c's /proc/[pid]/stat
     * vsize/rss fields, the only consumer. task_fork() copies this
     * unchanged (the child's address space is a real copy of the same
     * size until it execs something else). */
    uint32_t mapped_bytes;
    /* CPU time accounting, in PIT ticks (100 Hz — see arch/i386/pit.c) —
     * incremented for whichever task is `current` on every timer IRQ. */
    uint64_t cpu_ticks;
    /* Ticks accumulated since this task last became `current` — reset by
     * task_yield() whenever it switches to a new task. Used by pit_irq()'s
     * minimal preemption check (arch/i386/pit.c) to force a reschedule
     * once a CPU-bound task has monopolized the CPU for one quantum,
     * without this kernel needing a full priority/vruntime scheduler. */
    uint32_t quantum_ticks;
    /* Scheduling priority: -20 (highest) .. 19 (lowest), 0 default —
     * mirrors POSIX nice(). See kernel/task.c's next_ready_task(). */
    int nice;
    /* How many more scheduling passes this task is skipped before it's
     * eligible again — only ever nonzero for nice > 0 (see
     * next_ready_task()'s own comment for the exact scheme). Transient
     * scheduler bookkeeping, not part of the process's own identity —
     * reset to 0 whenever nice changes (task_setpriority()). */
    int skip_counter;

    /* Signals (kernel/signal.c, arch/i386/signal.c) — see
     * docs/process-management.md. Bit N of each mask corresponds to
     * signal number N (bit 0 unused; signal 0 is POSIX's null signal and
     * never actually delivered). */
    uint32_t pending_signals;
    uint32_t blocked_signals;
    /* Disposition per signal: 0 = SIG_DFL, 1 = SIG_IGN (matching newlib's
     * own SIG_DFL/SIG_IGN values exactly — see include/pureunix/signal.h),
     * else a ring-3 handler function pointer. Index 0 unused. */
    uint32_t sig_handlers[32];
    /* Nonzero while a real signal handler is executing (the signal number
     * currently being handled) — deliberately only one signal frame deep
     * (see docs/process-management.md's scope notes): no new handler is
     * ever invoked while this is nonzero, so a second catchable signal
     * arriving mid-handler simply stays pending until sigreturn. */
    int active_signal;
    /* Snapshot taken by arch/i386/signal.c's signal_deliver_pending() the
     * instant a handler is invoked, restored verbatim by its
     * signal_handle_sigreturn() (the int $0x82 trap) — this is what makes
     * "resume exactly where the signal interrupted" possible without
     * needing to round-trip any of it through the process's own stack. */
    interrupt_regs_t saved_regs_before_signal;
    uint32_t saved_blocked_before_signal;
    /* Which signal (SIGSTOP or SIGTSTP) most recently drove this task into
     * TASK_STOPPED — needed so waitpid(WUNTRACED)'s status encoding can
     * report the real stop signal via WSTOPSIG(), matching traditional
     * Unix job control. stop_reported is cleared on entering TASK_STOPPED
     * and set once a waitpid(WUNTRACED) call has reported this particular
     * stop episode, so a shell's `wait`/`fg` loop sees each distinct
     * stop/resume cycle exactly once rather than re-reporting the same
     * stop forever until the child is reaped. */
    int stop_signal;
    bool stop_reported;
    /* set for tasks produced by task_fork(): task_bootstrap() resumes them
     * via enter_usermode_regs(&fork_regs) instead of enter_usermode(), so
     * they continue exactly where their parent's fork() syscall was taken
     * (with fork_regs.eax already cleared to 0). */
    bool is_fork_child;
    interrupt_regs_t fork_regs;
    /* set by task_exit(), read back by task_join() */
    int exit_code;
    struct task *next;
    /* Intrusive link for whichever wait_queue_t this task is currently
     * queued on while TASK_SLEEPING (kernel/wait.c) — NULL otherwise. A
     * task can only ever be on one wait queue at a time in this design. */
    struct task *wait_next;
    /* Woken by kernel/task.c's task_exit() and kernel/signal.c's
     * signal_send() (every transition task_waitpid() might be waiting
     * on: a child becoming a zombie, entering TASK_STOPPED, or resuming
     * via SIGCONT) — task_waitpid() blocks here instead of the old
     * `task_yield()` busy-spin (M2 left this one call site unconverted;
     * M9's job control is what actually made the busy-spin's cost
     * visible — a shell polling waitpid() this way, with nothing else
     * runnable while every other VT session sleeps on its own input
     * queue, pegs the CPU at 100% for the entire duration of a
     * foreground job instead of ever truly blocking). */
    wait_queue_t child_wait;
    /* per-task file descriptor table; fds 0/1/2 are reserved (stdin/out/err) */
    fd_entry_t fds[MAX_OPEN_FILES];
    /* process credentials, used by the VFS permission engine (vfs_access).
     * No login system exists yet: the kernel task and everything it spawns
     * starts at uid=gid=0 (root) and a child inherits its creator's
     * credentials at task_create() time. */
    uid_t uid;
    gid_t gid;
    /* Which virtual terminal (0-based; see include/pureunix/vt.h) this
     * task's fd 0/1/2 default console binding and controlling-terminal
     * ioctls (VT_ACTIVATE/VT_GETACTIVE) target — -1 for tasks that predate
     * vt_init() (the initial kernel task, until kernel_main() claims VT1)
     * or otherwise aren't attached to any VT. Inherited from the creator at
     * task_alloc() time exactly like uid/gid/cwd below, which is what keeps
     * a shell and every process it launches (ping, seq, make, ...) bound to
     * the VT it was started on regardless of which VT is active later. */
    int vt_id;
    /* Working directory, used to resolve relative paths a syscall hands
     * the VFS (see SYS_CHDIR/SYS_GETCWD in arch/i386/syscall.c). Always an
     * absolute, normalized path (vfs_normalize()'s output). A child
     * inherits its creator's cwd at task_create()/task_fork() time — see
     * task_alloc() — exactly like uid/gid above. */
    char cwd[PUREUNIX_MAX_PATH];
} task_t;

void tasking_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg);
/* Like task_create(), but the task starts in ring 3 at `entry` running on
 * `user_stack_top` in the address space rooted at `pd_phys` (both must
 * already be mapped PAGE_USER within that directory) instead of calling a
 * kernel-mode function pointer. */
task_t *task_create_user(const char *name, uint32_t entry, uint32_t user_stack_top, uint32_t pd_phys);
/* Duplicates the calling task, which must be a ring-3 task: a new task_t,
 * a new kernel stack, a private copy of its address space (see
 * vmm_fork_address_space()), and a snapshot of *parent_regs (the int $0x80
 * trap frame) with eax cleared to 0 so the child "returns" 0 the first
 * time it's scheduled. Returns NULL on failure. */
task_t *task_fork(const interrupt_regs_t *parent_regs);
void task_yield(void);
/* True if some task other than `current` is TASK_RUNNABLE right now —
 * used by arch/i386/pit.c's minimal-preemption check to decide whether
 * forcing a reschedule would actually accomplish anything (no point
 * yielding away from a CPU-bound task if nothing else could use the
 * CPU anyway). */
bool task_other_runnable_exists(void);
void task_exit(int code);
/* Blocks (cooperatively, via task_yield) until `t` exits, then reaps it
 * (frees its stack and task_t) and returns its exit code. */
int task_join(task_t *t);
/* Closes every fd of t's with fd_entry_t.cloexec set — POSIX close-on-exec.
 * Called once by kernel/elf.c's elf_exec_current() (the exec() path that
 * keeps the caller's own fd table) after a new program image is loaded. */
void task_close_cloexec_fds(task_t *t);
/* Matches newlib's <sys/wait.h> WNOHANG/WUNTRACED exactly (see
 * third_party/newlib/i686-elf/include/sys/wait.h) so
 * user/newlib_syscalls.c's waitpid() can pass its own `options` argument
 * straight through the SYS_WAIT syscall with no translation. WNOHANG is
 * accepted but not yet implemented (task_waitpid() always blocks) — no
 * current caller needs non-blocking wait; flagged rather than silently
 * ignored so a future WNOHANG caller fails loudly instead of hanging. */
#define PU_WNOHANG   1
#define PU_WUNTRACED 2

/* pid == -1 waits for any child of the caller; pid > 0 waits for that one
 * specific child. Blocks (cooperatively) until a matching child becomes a
 * zombie, reaps it (freeing its stack, task_t, and private address space),
 * and returns its id. Returns -1 if the caller has no such child at all.
 * If status is non-NULL, the reaped child's exit code is written there.
 *
 * options is a PU_WNOHANG/PU_WUNTRACED bitmask. WUNTRACED additionally
 * makes a *live* child that just entered TASK_STOPPED (and hasn't already
 * been reported since its last stop) a match: *status is written using the
 * sentinel encoding documented on task_t.stop_signal's call sites
 * (kernel/signal.c) instead of the normal-exit/killed-by-signal encoding,
 * and the child is NOT reaped (it's still alive) — see docs/syscalls.md. */
int task_waitpid(int pid, int *status, int options);
task_t *task_current(void);
/* Looks up a live task by pid; NULL if no such task exists right now
 * (it may have already exited and been reaped). Never returns a pointer
 * that should be held onto across a yield — a task found this way can
 * exit and be reaped by the very next task_yield(). */
task_t *task_find(uint32_t id);
void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx);
/* Terminates task `id` (never the initial kernel task) with the POSIX
 * *default action* for signal `sig` — there is no handler-dispatch
 * mechanism (see docs/syscalls.md's SYS_KILL section for why), so every
 * nonzero `sig` just kills it outright, recorded as exit_code = -sig (see
 * SYS_WAIT's status convention below) so a waiting parent can tell a
 * signaled death from a normal exit. `sig == 0` is POSIX's "null signal":
 * probes whether `id` exists and is killable without killing it. Returns
 * 0 if `id` was found, -1 otherwise. */
int task_kill(uint32_t id, int sig);
/* True if any live (non-zombie) task has this pgid — used by SYS_KILL's
 * kill(-pgid, sig) existence check (arch/i386/syscall.c). */
bool task_pgrp_exists(uint32_t pgid);
/* If `t` is no longer TASK_RUNNING (a signal just terminated or stopped
 * it — see kernel/signal.c's signal_send()), yields away immediately
 * rather than letting its own code path keep executing on a stale
 * assumption that it's still runnable. Safe to call with t->state ==
 * TASK_RUNNING (no-op). Used by SYS_KILL's self-target case
 * (arch/i386/syscall.c) — a task that just signaled *itself* into
 * TASK_ZOMBIE/TASK_STOPPED must stop running right now, not merely the
 * next time it happens to yield. */
void task_yield_if_not_running(task_t *t);
/* Sets the pid every future orphan gets re-parented to (kernel/main.c,
 * once at boot, right after creating the dedicated init-reaper task —
 * see docs/process-management.md). */
void task_set_init_pid(uint32_t pid);

/* Process groups and sessions (POSIX setpgid()/getpgid()/setsid()/
 * getsid()). See docs/process-management.md for the exact rules and the
 * deliberate simplifications versus full POSIX (no cross-session-group
 * existence check, no EACCES-after-exec() race window enforcement — this
 * kernel's fork()+exec() model doesn't have the same TOCTOU surface a
 * real UNIX's does there).
 *
 * task_setpgid: pid == 0 means the caller; pgid == 0 means "use pid's own
 * id as the new pgid" (i.e. make it a new group leader). Only the caller
 * itself or a direct child of the caller may be retargeted, and only into
 * the caller's own session (setpgid() can move a process between groups
 * within a session, never across sessions). Returns 0, or a negative
 * errno (-ESRCH: no such pid; -EPERM: pid isn't caller or caller's child,
 * or pid is a session leader — POSIX forbids changing a session leader's
 * own group). */
int task_setpgid(uint32_t pid, uint32_t pgid);
/* pid == 0 means the caller. Returns the pgid, or -ESRCH. */
int task_getpgid(uint32_t pid);
/* Creates a new session and process group, both equal to the caller's own
 * id, with the caller as leader of both. Fails with -EPERM if the caller
 * is already a process group leader (pgid == its own id) — POSIX
 * forbids a group leader from starting a new session, since that would
 * leave the old group leaderless while the leader itself is still
 * running. Returns the new session id, or the negative errno. */
int task_setsid(void);
/* pid == 0 means the caller. Returns the sid, or -ESRCH. */
int task_getsid(uint32_t pid);

/* nice()/renice() — see kernel/task.c's next_ready_task() for how this
 * actually biases scheduling. pid == 0 means the caller. Clamps nice to
 * [-20, 19] (POSIX). No privilege check — see task_setpriority()'s own
 * comment. Returns 0, or -ESRCH. */
int task_setpriority(uint32_t pid, int nice);
/* pid == 0 means the caller. Writes the current nice value to *out_nice
 * (if non-NULL) and returns 0, or returns -ESRCH — deliberately never
 * returns the nice value directly as the function result, since a valid
 * nice value can itself be negative and collide with a negative-errno
 * convention (the same reason real getpriority() requires callers to
 * clear errno first rather than just checking the return value). */
int task_getpriority(uint32_t pid, int *out_nice);

/* Credentials of the currently scheduled task. Never reads a global —
 * always goes through task_current(), so callers (chiefly the VFS
 * permission engine) see whichever task is actually running. */
uid_t current_uid(void);
gid_t current_gid(void);

/* Sets the *current* task's credentials outright, no privilege check —
 * called only by the kernel-mode login flow (kernel/users.c) once a
 * username/password pair has been verified against /etc/shadow. */
void task_set_creds(uid_t uid, gid_t gid);

/* Working directory of the currently scheduled task — same current()-
 * indirection rationale as current_uid()/current_gid() above. Always an
 * absolute, normalized path; "/" for the initial kernel task. */
const char *task_current_cwd(void);
/* Sets the *current* task's cwd outright (SYS_CHDIR has already resolved
 * and validated path as a real, accessible directory — see
 * arch/i386/syscall.c). Also used by shell/sh.c to keep the kernel's own
 * "current" task (there is no separate task for the interactive shell — it
 * runs as the same task that calls kernel_main()) in sync with the shell's
 * own cwd tracking, so a child process spawned via elf_exec_argv() starts
 * in the directory the shell was actually in, not always "/". Returns -1 if
 * path doesn't fit in the task's cwd buffer. */
int task_set_cwd(const char *path);

#endif
