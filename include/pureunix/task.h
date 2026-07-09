#ifndef PUREUNIX_TASK_H
#define PUREUNIX_TASK_H

#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/types.h>

#define MAX_OPEN_FILES 16

typedef enum fd_kind {
    FD_KIND_FILE = 0,
    FD_KIND_PIPE = 1,
    /* A /dev/ttyN (or /dev/tty) descriptor opened explicitly via SYS_OPEN
     * (arch/i386/syscall.c's /dev/tty interception) — distinct from fd
     * 0/1/2's *default* console binding (file == NULL, see fd_entry_t
     * below), which never allocates an open_file_t at all. */
    FD_KIND_TTY = 2,
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
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
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
    /* creator task, used by task_waitpid() to find a caller's own children */
    struct task *parent;
    /* set for tasks produced by task_fork(): task_bootstrap() resumes them
     * via enter_usermode_regs(&fork_regs) instead of enter_usermode(), so
     * they continue exactly where their parent's fork() syscall was taken
     * (with fork_regs.eax already cleared to 0). */
    bool is_fork_child;
    interrupt_regs_t fork_regs;
    /* set by task_exit(), read back by task_join() */
    int exit_code;
    struct task *next;
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
void task_exit(int code);
/* Blocks (cooperatively, via task_yield) until `t` exits, then reaps it
 * (frees its stack and task_t) and returns its exit code. */
int task_join(task_t *t);
/* pid == -1 waits for any child of the caller; pid > 0 waits for that one
 * specific child. Blocks (cooperatively) until a matching child becomes a
 * zombie, reaps it (freeing its stack, task_t, and private address space),
 * and returns its id. Returns -1 if the caller has no such child at all.
 * If status is non-NULL, the reaped child's exit code is written there. */
int task_waitpid(int pid, int *status);
task_t *task_current(void);
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
