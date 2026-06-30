#ifndef PUREUNIX_TASK_H
#define PUREUNIX_TASK_H

#include <pureunix/config.h>
#include <pureunix/types.h>

#define MAX_OPEN_FILES 16

/* One slot in a task's file descriptor table.
   data is the entire file loaded at open() time via vfs_read_file(). */
typedef struct fd_entry {
    bool     used;
    char     path[PUREUNIX_MAX_PATH];
    uint8_t *data;    /* kmalloc'd file contents; freed on close */
    size_t   size;
    size_t   offset;  /* current read position */
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
    struct task *next;
    /* per-task file descriptor table; fds 0/1/2 are reserved (stdin/out/err) */
    fd_entry_t fds[MAX_OPEN_FILES];
} task_t;

void tasking_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg);
void task_yield(void);
void task_exit(void);
task_t *task_current(void);
void task_list(void (*cb)(const task_t *task, void *ctx), void *ctx);
int task_kill(uint32_t id);

#endif
