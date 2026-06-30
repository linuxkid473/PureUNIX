#ifndef PUREUNIX_TASK_H
#define PUREUNIX_TASK_H

#include <pureunix/types.h>

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
    struct task *next;
} task_t;

void tasking_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg);
void task_yield(void);
void task_exit(void);
task_t *task_current(void);
void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx);
int task_kill(uint32_t id);

#endif
