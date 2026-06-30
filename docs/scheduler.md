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
`TASK_ZOMBIE` tasks are never reaped; their `task_t` structure and stack remain allocated indefinitely.

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
    current         = &main_task;
    task_list_head  = &main_task;
}
```

`main_task` is a static variable; it does not need a separately allocated stack because it continues using the boot stack already in place.

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
    if (current && current->entry) {
        current->entry(current->arg);
    }
    task_exit();
}
```

When a newly created task runs for the first time, `context_switch` "returns" to `task_bootstrap`, which calls the task's entry function. When the entry function returns, `task_bootstrap` calls `task_exit()`.

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
6. Calls `context_switch`.

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
- **No zombie reaping**: memory for exited tasks leaks permanently.
- **`SYS_EXIT` does not terminate**: the syscall returns the EBX value to the caller but does not call `task_exit`. ELF programs that return from `main` will return through `crt0.S` (`ret` in `_start`), which returns from the `elf_exec` call rather than running `task_exit`.
- **Stack overflow**: there is no guard page below task stacks. Overflow silently corrupts adjacent heap memory.
- **No priority**: all tasks have equal priority in the round-robin.
