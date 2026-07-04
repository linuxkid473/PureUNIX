#include <pureunix/arch.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>

#define TASK_STACK_SIZE 16384

static task_t main_task;
static task_t *current;
static task_t *task_list_head;
static uint32_t next_task_id = 1;

static void task_bootstrap(void)
{
    if (current) {
        if (current->is_user) {
            enter_usermode(current->user_entry, current->user_stack);
        } else if (current->entry) {
            current->entry(current->arg);
        }
    }
    task_exit(0);
}

static void reserve_stdio(task_t *task)
{
    /* fds 0/1/2 are stdin/stdout/stderr; handled by SYS_READ/SYS_WRITE */
    task->fds[0].used = true;
    task->fds[1].used = true;
    task->fds[2].used = true;
}

void tasking_init(void)
{
    memset(&main_task, 0, sizeof(main_task));
    main_task.id = next_task_id++;
    strcpy(main_task.name, "kernel");
    main_task.state = TASK_RUNNING;
    main_task.next = &main_task;
    reserve_stdio(&main_task);
    main_task.uid = 0;
    main_task.gid = 0;
    current = &main_task;
    task_list_head = &main_task;
}

static task_t *task_alloc(const char *name)
{
    task_t *task = kcalloc(1, sizeof(task_t));
    if (!task) {
        return NULL;
    }
    task->stack_base = kmalloc(TASK_STACK_SIZE);
    if (!task->stack_base) {
        kfree(task);
        return NULL;
    }
    task->id = next_task_id++;
    strncpy(task->name, name ? name : "task", sizeof(task->name) - 1);
    task->state = TASK_READY;
    reserve_stdio(task);
    /* Credentials propagate from creator to child — the only "process
     * spawning" rule that exists before a real login/setuid model arrives. */
    task->uid = current ? current->uid : 0;
    task->gid = current ? current->gid : 0;

    uint32_t *sp = (uint32_t *)(task->stack_base + TASK_STACK_SIZE);
    *--sp = (uint32_t)task_bootstrap;
    *--sp = 0x202;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    task->stack_ptr = sp;

    task_t *tail = task_list_head;
    while (tail->next != task_list_head) {
        tail = tail->next;
    }
    tail->next = task;
    task->next = task_list_head;
    return task;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg)
{
    task_t *task = task_alloc(name);
    if (!task) {
        return NULL;
    }
    task->entry = entry;
    task->arg = arg;
    return task;
}

task_t *task_create_user(const char *name, uint32_t entry, uint32_t user_stack_top)
{
    task_t *task = task_alloc(name);
    if (!task) {
        return NULL;
    }
    task->is_user = true;
    task->user_entry = entry;
    task->user_stack = user_stack_top;
    return task;
}

static task_t *next_ready_task(void)
{
    task_t *candidate = current->next;
    while (candidate != current) {
        if (candidate->state == TASK_READY || candidate->state == TASK_RUNNING) {
            return candidate;
        }
        candidate = candidate->next;
    }
    return current;
}

void task_yield(void)
{
    task_t *prev = current;
    task_t *next = next_ready_task();
    if (next == prev) {
        return;
    }
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    current = next;
    /* main_task runs on the boot stack (no stack_base) and never executes
     * in ring 3, so it has nothing to install here. */
    if (next->stack_base) {
        tss_set_kernel_stack((uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }
    context_switch(&prev->stack_ptr, next->stack_ptr);
}

void task_exit(int code)
{
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
    task_yield();
    for (;;) {
        arch_halt();
    }
}

int task_join(task_t *t)
{
    if (!t) {
        return -1;
    }
    while (t->state != TASK_ZOMBIE) {
        task_yield();
    }
    int code = t->exit_code;

    task_t *prev = task_list_head;
    while (prev->next != t) {
        prev = prev->next;
    }
    prev->next = t->next;

    kfree(t->stack_base);
    kfree(t);
    return code;
}

task_t *task_current(void)
{
    return current;
}

uid_t current_uid(void)
{
    return current ? current->uid : 0;
}

gid_t current_gid(void)
{
    return current ? current->gid : 0;
}

void task_set_creds(uid_t uid, gid_t gid)
{
    if (current) {
        current->uid = uid;
        current->gid = gid;
    }
}

void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx)
{
    if (!task_list_head || !cb) {
        return;
    }
    task_t *task = task_list_head;
    do {
        cb(task, ctx);
        task = task->next;
    } while (task && task != task_list_head);
}

int task_kill(uint32_t id)
{
    task_t *task = task_list_head;
    do {
        if (task->id == id && task != &main_task) {
            task->state = TASK_ZOMBIE;
            return 0;
        }
        task = task->next;
    } while (task && task != task_list_head);
    return -1;
}
