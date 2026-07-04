#ifndef PUREUNIX_TASK_H
#define PUREUNIX_TASK_H

#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/types.h>

#define MAX_OPEN_FILES 16

/* One slot in a task's file descriptor table.
   data is the entire file loaded at open() time via vfs_read_file(). */
typedef struct fd_entry {
    bool     used;
    int      flags;              /* open() flags: O_RDONLY / O_WRONLY / O_RDWR */
    char     path[PUREUNIX_MAX_PATH]; /* VFS path — used for re-stat and debugging */
    uint8_t *data;               /* kmalloc'd file contents; freed on close */
    size_t   size;               /* total file size in bytes */
    size_t   offset;             /* current seek position */
} fd_entry_t;

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
int task_kill(uint32_t id);

/* Credentials of the currently scheduled task. Never reads a global —
 * always goes through task_current(), so callers (chiefly the VFS
 * permission engine) see whichever task is actually running. */
uid_t current_uid(void);
gid_t current_gid(void);

/* Sets the *current* task's credentials outright, no privilege check —
 * called only by the kernel-mode login flow (kernel/users.c) once a
 * username/password pair has been verified against /etc/shadow. */
void task_set_creds(uid_t uid, gid_t gid);

#endif
