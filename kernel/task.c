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
    if (current && current->entry) {
        current->entry(current->arg);
    }
    task_exit();
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
    current = &main_task;
    task_list_head = &main_task;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg)
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
    task->entry = entry;
    task->arg = arg;
    reserve_stdio(task);

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
    context_switch(&prev->stack_ptr, next->stack_ptr);
}

void task_exit(void)
{
    current->state = TASK_ZOMBIE;
    task_yield();
    for (;;) {
        arch_halt();
    }
}

task_t *task_current(void)
{
    return current;
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
