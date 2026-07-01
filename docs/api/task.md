# API Reference: Task / Scheduler

**Header**: `<pureunix/task.h>`

---

## Types

### `task_state_t`

```c
typedef enum task_state {
    TASK_READY,     // eligible to run
    TASK_RUNNING,   // currently executing
    TASK_SLEEPING,  // defined but unused; no wake mechanism exists
    TASK_ZOMBIE,    // exited; will not be scheduled; memory not reclaimed
} task_state_t;
```

### `task_t`

```c
typedef struct task {
    uint32_t      id;           // unique task ID, starting at 1
    char          name[32];     // human-readable name
    task_state_t  state;
    uint32_t     *stack_ptr;    // saved ESP; written/read by context_switch
    uint8_t      *stack_base;   // base of 16 KiB stack allocation
    void        (*entry)(void *); // entry function
    void         *arg;          // passed to entry on first run
    struct task  *next;         // next node in circular list
    fd_entry_t    fds[MAX_OPEN_FILES]; // per-task file descriptor table
    uid_t         uid;          // process credentials (Stage 3A)
    gid_t         gid;
} task_t;
```

### Credentials (Stage 3A)

`uid`/`gid` are the process credentials consumed by the VFS permission engine (`vfs_access`, see `docs/api/vfs.md`). `tasking_init()` sets the initial kernel task to `uid=0, gid=0` (root); `task_create()` copies the *creating* task's credentials into the new task — the only credential-inheritance rule that exists before a real login/setuid model arrives. There is no login system, no additional users, and no passwd database yet, so in practice every task is root.

```c
uid_t current_uid(void);   // task_current()->uid, or 0 if there is no current task
gid_t current_gid(void);   // task_current()->gid, or 0 if there is no current task
```

Never read from a global — both always go through `task_current()`, so they reflect whichever task is actually scheduled.

---

## Functions

### `tasking_init`

```c
void tasking_init(void);
```

Creates the initial `kernel` task from the current execution context. The kernel task uses the boot stack directly and does not need an allocated stack. Sets `current` to the kernel task. Must be called once before `task_create`.

### `task_create`

```c
task_t *task_create(const char *name, void (*entry)(void *), void *arg);
```

Allocates a `task_t` and a 16 KiB stack. Initializes the stack so that the first context switch to this task begins executing `task_bootstrap(entry, arg)`. Appends the task to the circular ready list. Returns a pointer to the new task, or NULL if allocation fails.

### `task_yield`

```c
void task_yield(void);
```

Finds the next `TASK_READY` or `TASK_RUNNING` task in the circular list. If one is found, transitions the current task to `TASK_READY`, transitions the next task to `TASK_RUNNING`, updates `current`, and calls `context_switch`. If no other ready task exists, returns without switching.

### `task_exit`

```c
void task_exit(void);
```

Sets the current task's state to `TASK_ZOMBIE` and calls `task_yield`. If `task_yield` returns (which should not happen), loops on `arch_halt()`. Does not free any task memory.

### `task_current`

```c
task_t *task_current(void);
```

Returns a pointer to the currently executing task.

### `task_list`

```c
void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx);
```

Walks the entire circular task list and calls `cb` for each task. The callback receives a read-only pointer to each `task_t` and the caller-supplied `ctx`. Used by the `ps` shell builtin.

### `task_kill`

```c
int task_kill(uint32_t id);
```

Sets the state of task with the given `id` to `TASK_ZOMBIE`. Refuses to kill the initial kernel task (id 1). Returns `0` on success, `-1` if the task is not found.

---

## Context Switch (low-level)

**Header**: `<pureunix/arch.h>`

```c
void context_switch(uint32_t **old_sp, uint32_t *new_sp);
```

Saves EFLAGS and all general-purpose registers onto the current stack, writes the current ESP to `*old_sp`, loads `new_sp` into ESP, then restores the new task's registers and returns. Not called directly; use `task_yield`.
