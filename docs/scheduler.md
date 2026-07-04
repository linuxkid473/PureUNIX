# Scheduler

## Overview

PureUnix uses a **cooperative, round-robin** scheduler. Tasks must explicitly yield the CPU; there is no preemptive timer-driven context switch. The scheduler is implemented in `kernel/task.c` and the context switch primitive is in `arch/i386/context_switch.S`.

---

## Task Structure

```c
typedef struct task {
    uint32_t        id;           // unique, assigned at creation (starts at 1)
    char            name[32];     // human-readable name, truncated if necessary
    task_state_t    state;        // current state
    uint32_t       *stack_ptr;    // saved stack pointer (used by context_switch)
    uint8_t        *stack_base;   // base of the allocated stack buffer
    void          (*entry)(void *); // entry function
    void           *arg;          // argument passed to entry
    bool            is_user;      // started via task_create_user()/task_fork()
    uint32_t        user_entry;
    uint32_t        user_stack;
    uint32_t        pd_phys;      // this task's page directory (CR3 value) —
                                   // see docs/memory.md's per-process VMM section
    struct task    *parent;       // creator, used by task_waitpid()
    bool            is_fork_child;   // resume via enter_usermode_regs(), not enter_usermode()
    interrupt_regs_t fork_regs;      // snapshot of the parent's int $0x80 trap frame
    int             exit_code;
    struct task    *next;         // next task in circular linked list
} task_t;
```

### Task States

```c
typedef enum task_state {
    TASK_READY,      // eligible to run; will be selected by next_ready_task
    TASK_RUNNING,    // currently executing
    TASK_SLEEPING,   // defined but not used; no wake-up mechanism exists
    TASK_ZOMBIE,     // has exited; will not be scheduled; memory not reclaimed
} task_state_t;
```

`TASK_SLEEPING` is defined in the enum but no code path sets a task to this state.  
`TASK_ZOMBIE` tasks are reaped by whichever call actually claims them — `task_join()` (used internally by `elf_exec()`) or `task_waitpid()` (the `SYS_WAIT` syscall) — which frees the stack, the private page directory (if any), and the `task_t` itself. A zombie nobody ever joins/waits for (e.g. one killed via `task_kill()`) is still never reaped — that part of the limitation stands.

---

## Initialization

`tasking_init()` creates the initial "kernel" task from the current execution context:

```c
static task_t main_task;

void tasking_init(void) {
    memset(&main_task, 0, sizeof(main_task));
    main_task.id    = next_task_id++;  // id = 1
    strcpy(main_task.name, "kernel");
    main_task.state = TASK_RUNNING;
    main_task.next  = &main_task;      // circular list, one element
    main_task.pd_phys = vmm_kernel_directory_phys();
    current         = &main_task;
    task_list_head  = &main_task;
}
```

`main_task` is a static variable; it does not need a separately allocated stack because it continues using the boot stack already in place. Its `pd_phys` is the kernel's own page directory — every kernel-mode task (`task_create()`) inherits its creator's `pd_phys`, so only tasks started via `task_create_user()`/`task_fork()` ever run with a different (private) one.

---

## Task Creation

`task_create(name, entry, arg)`:

1. Allocates a `task_t` via `kcalloc`.
2. Allocates a 16 KiB stack via `kmalloc`.
3. Initializes the stack frame so that when `context_switch` first resumes this task, it begins executing `task_bootstrap`.

The stack setup places the following in memory growing downward from `stack_base + TASK_STACK_SIZE`:

```
[top]
    address of task_bootstrap  <- return address popped by context_switch
    0x202                      <- EFLAGS (IF=1, reserved bit set)
    0 (EDI)
    0 (ESI)
    0 (EBP)
    0 (ESP)
    0 (EBX)
    0 (EDX)
    0 (ECX)
    0 (EAX)
[task->stack_ptr points here]
```

This matches the layout that `context_switch` expects (see below).

4. Appends the task to the circular list after the last task.

---

## Task Bootstrap

```c
static void task_bootstrap(void) {
    if (current) {
        if (current->is_fork_child) {
            enter_usermode_regs(&current->fork_regs);
        } else if (current->is_user) {
            enter_usermode(current->user_entry, current->user_stack);
        } else if (current->entry) {
            current->entry(current->arg);
        }
    }
    task_exit(0);
}
```

When a newly created task runs for the first time, `context_switch` "returns" to `task_bootstrap`. Three cases:
- **`is_fork_child`** (produced by `task_fork()`): `enter_usermode_regs()` restores the full register/segment/iret frame captured from the parent's `int $0x80` trap (with `eax` already zeroed by `task_fork()`) and drops to ring 3 exactly where the parent's `fork()` call was made — this is how `fork()` "returns twice".
- **`is_user`** (produced by `task_create_user()`, e.g. by `elf_exec()`): `enter_usermode()` drops to ring 3 at `user_entry` on `user_stack`, a fresh process with no inherited register state.
- Otherwise (produced by `task_create()`): calls the kernel-mode `entry(arg)` function pointer directly.

Whichever path is taken, when it returns `task_bootstrap` calls `task_exit(0)`.

---

## Context Switch

`arch/i386/context_switch.S`:

```asm
context_switch:
    mov 4(%esp), %eax   ; eax = &old_sp  (pointer to old task's stack_ptr field)
    mov 8(%esp), %edx   ; edx = new_sp   (new task's saved stack pointer value)
    pushfl              ; save EFLAGS
    pusha               ; save EAX ECX EDX EBX ESP EBP ESI EDI
    mov %esp, (%eax)    ; old_sp->stack_ptr = current ESP
    mov %edx, %esp      ; ESP = new task's stack_ptr
    popa                ; restore new task's registers
    popfl               ; restore EFLAGS
    ret                 ; return to new task's saved EIP
```

The switch saves and restores EFLAGS (including the IF flag) and all general-purpose registers. Segment registers are not saved or restored; all tasks share the same GDT selectors.

### Signature

```c
void context_switch(uint32_t **old_sp, uint32_t *new_sp);
```

- `old_sp`: pointer to `current->stack_ptr` — the current ESP is written here.
- `new_sp`: the value of `next->stack_ptr` — ESP is loaded from this.

---

## Yielding

`task_yield()`:

1. Finds the next ready task via `next_ready_task()`.
2. If no other ready task exists, returns without switching.
3. Sets the previous task from `TASK_RUNNING` to `TASK_READY`.
4. Sets the next task to `TASK_RUNNING`.
5. Updates `current`.
6. If `next->pd_phys != prev->pd_phys`, calls `vmm_switch_directory(next->pd_phys)` — this is what makes address spaces actually change when switching to/from a user-mode process; skipped when both tasks share a directory (e.g. switching between two kernel tasks) to avoid a needless CR3 reload/TLB flush.
7. Calls `context_switch`.

`next_ready_task()` performs a linear scan of the circular list starting from `current->next`, returning the first task in `TASK_READY` or `TASK_RUNNING` state, or `current` if none is found.

---

## Task Exit

`task_exit()`:

```c
void task_exit(void) {
    current->state = TASK_ZOMBIE;
    task_yield();
    for (;;) { arch_halt(); }
}
```

Sets the current task to ZOMBIE and yields. If `task_yield` ever returns (it should not, since ZOMBIE tasks are not selected), the function halts in a loop.

**No reaping**: zombie tasks are never removed from the list and their memory is never freed. This is a known limitation.

---

## Forking

`task_fork(parent_regs)` (called by the `SYS_FORK` handler with the caller's own `int $0x80` trap frame) duplicates the calling task, which must already be a ring-3 (`is_user`) task:

1. `task_alloc()` a new `task_t` + kernel stack, same as any other task.
2. `vmm_create_user_directory()` + `vmm_fork_address_space()` give the child a private, deep copy of the parent's user window (see `docs/memory.md`) — parent and child are independent from this point on.
3. `is_fork_child = true` and `fork_regs = *parent_regs` with `fork_regs.eax` cleared to 0, so the child "returns" 0 from `fork()` the first time it's scheduled (see Task Bootstrap above); the parent gets the child's id back from the normal syscall return path.
4. Every fd the parent has open is deep-copied (not shared) — this kernel has no shared-file-description concept, so seeking/closing one process's copy never affects the other's.

Returns the new `task_t *`, or `NULL` on failure (not a ring-3 caller, or allocation/address-space-copy failure).

---

## Waiting

`task_waitpid(pid, status)` (the `SYS_WAIT` handler): `pid == -1` waits for any child of the caller; `pid > 0` waits for that specific child (verified via `parent == current`, not just a matching id). Scans the circular list; if a matching child is already a zombie, reaps it immediately (same cleanup as `task_join()` — stack, private directory, `task_t`) and returns its id with `*status` set to its exit code. If a matching child exists but isn't a zombie yet, calls `task_yield()` and rescans. If the caller has no matching child at all, returns `-1` immediately rather than yielding forever.

---

## Killing Tasks

`task_kill(id)` walks the circular list and sets the target task's state to `TASK_ZOMBIE`. It refuses to kill `main_task` (the initial kernel task). Returns `0` on success, `-1` if the task is not found.

---

## Task Enumeration

```c
void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx);
```

Walks the entire circular list and calls `cb` for each task. Used by the `ps` shell builtin.

---

## Limitations

- **No preemption**: the PIT IRQ0 handler only increments a tick counter. It does not call `task_yield`. A task that does not call `task_yield` or block on I/O will monopolize the CPU indefinitely.
- **TASK_SLEEPING unused**: the state is defined but no mechanism puts a task to sleep or wakes it up.
- **Zombie reaping requires a waiter**: `task_join()`/`task_waitpid()` reap a zombie when something actually claims it; a zombie nobody joins/waits for (e.g. `task_kill()`'s target) is never reaped.
- **`SYS_EXIT` does not terminate**: the syscall returns the EBX value to the caller but does not call `task_exit`. ELF programs that return from `main` will return through `crt0.S` (`ret` in `_start`), which returns from the `elf_exec` call rather than running `task_exit`. `pu_exit()` (userspace) and the `int $0x81` trap it wraps are the real termination path — see `docs/syscalls.md`.
- **Stack overflow**: there is no guard page below task stacks. Overflow silently corrupts adjacent heap memory.
- **No priority**: all tasks have equal priority in the round-robin.
- **fork() deep-copies fds, not shared file descriptions**: unlike POSIX (where a forked fd shares its file offset with the parent's), each side gets its own independent copy of the buffered file content and offset.
