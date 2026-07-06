#include <pureunix/arch.h>
#include <pureunix/fcntl.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>
#include <pureunix/vmm.h>

/* Every syscall — including deep VFS/filesystem call chains — runs on the
 * calling task's own kernel stack (there is no separate per-syscall stack).
 * 16 KiB (the original value here) turned out to be too tight: EXT2's own
 * internal functions each keep a full block-sized local buffer on the
 * stack rather than sharing one (fs/ext2/alloc.c's ext2_alloc_block() alone
 * stacks two 4 KiB buffers), and the deepest real call chain — creating a
 * file that triggers directory growth, e.g. SYS_OPEN -> vfs_create() ->
 * ext2_create() -> ext2_dir_insert() -> ext2_alloc_block() — stacks three
 * or more of these on top of each other, on top of the interrupt frame and
 * every function's own smaller locals. That silently overran a 16 KiB
 * stack under user/systest.c's 200-file directory-growth stress section
 * (corrupting whatever kernel memory happened to sit past the stack, with
 * no guard page to catch it) — found via this exact repro while adding
 * SYS_PIPE/SYS_DUP/SYS_DUP2. Quadrupling the budget leaves comfortable
 * headroom for this and any similarly deep future call chain. */
#define TASK_STACK_SIZE 65536

static task_t main_task;
static task_t *current;
static task_t *task_list_head;
static uint32_t next_task_id = 1;

static void task_bootstrap(void)
{
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

static void reserve_stdio(task_t *task)
{
    /* fds 0/1/2 are stdin/stdout/stderr; handled by SYS_READ/SYS_WRITE.
     * task is kcalloc'd, so fds[0..2].file already starts NULL — the
     * default console binding (see include/pureunix/task.h's fd_entry_t
     * comment) — with nothing further to set up here. */
    task->fds[0].used = true;
    task->fds[1].used = true;
    task->fds[2].used = true;
}

/* A static, system-wide pool rather than a kmalloc()/kfree() per open()
 * (this project's usual fixed-resource style — see MAX_OPEN_FILES itself):
 * open file descriptions come and go far more often than any other
 * kmalloc'd object in this kernel (once per open()/close(), not once per
 * file's lifetime the way the actual data buffer is), which is heap churn
 * this simple linked-list allocator (kernel/heap.c) wasn't exercised with
 * before — enough to reveal a heap accounting bug under the systest.c
 * stress section that creates/closes 200 files back to back. A free slot
 * is refcount == 0; open_file_unref() only ever resets a slot back to that
 * state; it never kfree()s the open_file_t itself (only the FD_KIND_FILE
 * data buffer / FD_KIND_PIPE ring buffer it may point to, both unaffected
 * by this). */
#define MAX_OPEN_FILE_DESCRIPTIONS 128
static open_file_t g_open_files[MAX_OPEN_FILE_DESCRIPTIONS];

open_file_t *open_file_alloc(fd_kind_t kind)
{
    for (int i = 0; i < MAX_OPEN_FILE_DESCRIPTIONS; i++) {
        if (g_open_files[i].refcount == 0) {
            memset(&g_open_files[i], 0, sizeof(g_open_files[i]));
            g_open_files[i].refcount = 1;
            g_open_files[i].kind = kind;
            return &g_open_files[i];
        }
    }
    return NULL;
}

void open_file_ref(open_file_t *f)
{
    if (f) {
        f->refcount++;
    }
}

int open_file_unref(open_file_t *f)
{
    if (!f) {
        return 0;
    }
    if (--f->refcount > 0) {
        return 0;
    }
    int rc = 0;
    if (f->kind == FD_KIND_FILE) {
        if (f->flags & O_WRONLY) {
            rc = vfs_write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size, 0);
        }
        kfree(f->data);
    } else { /* FD_KIND_PIPE */
        pipe_buf_t *p = f->pipe_buf;
        if (p) {
            if (f->pipe_is_write_end) {
                p->write_ends--;
            } else {
                p->read_ends--;
            }
            if (p->read_ends == 0 && p->write_ends == 0) {
                kfree(p);
            }
        }
    }
    /* f itself is a slot in the static g_open_files[] pool, not a kmalloc'd
     * block — nothing to free; refcount == 0 (already true here) is what
     * marks it available for open_file_alloc() to reuse. */
    return rc;
}

void tasking_init(void)
{
    memset(&main_task, 0, sizeof(main_task));
    main_task.id = next_task_id++;
    strcpy(main_task.name, "kernel");
    main_task.state = TASK_RUNNING;
    main_task.next = &main_task;
    main_task.pd_phys = vmm_kernel_directory_phys();
    reserve_stdio(&main_task);
    main_task.uid = 0;
    main_task.gid = 0;
    strcpy(main_task.cwd, "/");
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
    strcpy(task->cwd, current && current->cwd[0] ? current->cwd : "/");
    task->parent = current;
    task->pd_phys = current ? current->pd_phys : vmm_kernel_directory_phys();

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

task_t *task_create_user(const char *name, uint32_t entry, uint32_t user_stack_top, uint32_t pd_phys)
{
    task_t *task = task_alloc(name);
    if (!task) {
        return NULL;
    }
    task->is_user = true;
    task->user_entry = entry;
    task->user_stack = user_stack_top;
    task->pd_phys = pd_phys;

    /* Shares (not deep-copies — same reasoning as task_fork(), see there)
     * whatever fds the creator currently has open. This is what lets the
     * kernel shell (shell/sh.c — there's no separate task for it; it runs
     * as the same task that calls kernel_main(), i.e. "current" here)
     * redirect a launched program's stdout/stdin by dup2()ing something
     * onto its own fd 0/1 *before* calling elf_exec_argv(): the new
     * process starts with that same binding already in place, exactly
     * like a real shell's fork()+dup2()+exec() sequence achieves the same
     * result for a program that manages its own fd table. */
    if (current) {
        for (int i = 0; i < MAX_OPEN_FILES; ++i) {
            if (!current->fds[i].used) {
                continue;
            }
            task->fds[i].used = true;
            task->fds[i].file = current->fds[i].file;
            open_file_ref(task->fds[i].file);
        }
    }
    return task;
}

static void unlink_task(task_t *t)
{
    task_t *prev = task_list_head;
    while (prev->next != t) {
        prev = prev->next;
    }
    prev->next = t->next;
}

task_t *task_fork(const interrupt_regs_t *parent_regs)
{
    if (!current || !current->is_user) {
        return NULL;
    }

    task_t *child = task_alloc(current->name);
    if (!child) {
        return NULL;
    }

    uint32_t child_pd = vmm_create_user_directory();
    if (!child_pd || !vmm_fork_address_space(child_pd, current->pd_phys)) {
        if (child_pd) {
            vmm_free_user_directory(child_pd);
        }
        unlink_task(child);
        kfree(child->stack_base);
        kfree(child);
        return NULL;
    }

    child->is_user = true;
    child->pd_phys = child_pd;
    child->user_entry = current->user_entry;
    child->user_stack = current->user_stack;
    child->is_fork_child = true;
    child->fork_regs = *parent_regs;
    child->fork_regs.eax = 0; /* fork() returns 0 in the child */

    /* Share open file descriptions with the child, per POSIX fork() —
     * including the current seek offset, since both fd table slots now
     * point at the very same open_file_t (see include/pureunix/task.h) —
     * rather than the deep copy this used to do (which gave the child an
     * independent offset on every inherited fd, not real fork() semantics).
     * This is also what makes a pipe's two ends survive a fork() intact:
     * both ends are just fd-table entries like any other, and fork() must
     * bump their open_file_t's refcount exactly like dup()/dup2() do, or
     * a later close() in either parent or child would free the pipe out
     * from under the other. */
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!current->fds[i].used) {
            continue;
        }
        child->fds[i].used = true;
        child->fds[i].file = current->fds[i].file;
        open_file_ref(child->fds[i].file);
    }

    return child;
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
    if (next->pd_phys != prev->pd_phys) {
        vmm_switch_directory(next->pd_phys);
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

/* Releases every fd a dying task still holds a reference through
 * (open_file_unref() flushes a written FD_KIND_FILE / retires a
 * FD_KIND_PIPE end once this was the last reference — see
 * include/pureunix/task.h) — called from both reap paths below, since a
 * task can become a zombie via a normal exit (task_exit(), which runs in
 * the exiting task's own context) *or* via task_kill() (which sets
 * another task's state directly, with no chance to run cleanup code in
 * that task's own context) — reaping is the one place both converge.
 * Without this, any fd a process never explicitly close()s before dying
 * would permanently leak its slot in the fixed-size open-file-description
 * pool (kernel/task.c's g_open_files[]). */
static void close_all_fds(task_t *t)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (t->fds[i].used && t->fds[i].file) {
            open_file_unref(t->fds[i].file);
            t->fds[i].file = NULL;
        }
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

    unlink_task(t);
    close_all_fds(t);
    if (t->pd_phys != vmm_kernel_directory_phys()) {
        vmm_free_user_directory(t->pd_phys);
    }
    kfree(t->stack_base);
    kfree(t);
    return code;
}

int task_waitpid(int pid, int *status)
{
    for (;;) {
        bool have_child = false;
        task_t *t = task_list_head;
        do {
            if (t->parent == current && (pid == -1 || (int)t->id == pid)) {
                have_child = true;
                if (t->state == TASK_ZOMBIE) {
                    int code = t->exit_code;
                    uint32_t reaped_id = t->id;

                    unlink_task(t);
                    close_all_fds(t);
                    if (t->pd_phys != vmm_kernel_directory_phys()) {
                        vmm_free_user_directory(t->pd_phys);
                    }
                    kfree(t->stack_base);
                    kfree(t);

                    if (status) {
                        *status = code;
                    }
                    return (int)reaped_id;
                }
            }
            t = t->next;
        } while (t != task_list_head);

        if (!have_child) {
            return -1;
        }
        task_yield();
    }
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

const char *task_current_cwd(void)
{
    return current && current->cwd[0] ? current->cwd : "/";
}

int task_set_cwd(const char *path)
{
    if (!current || !path || strlen(path) >= sizeof(current->cwd)) {
        return -1;
    }
    strcpy(current->cwd, path);
    return 0;
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

int task_kill(uint32_t id, int sig)
{
    task_t *task = task_list_head;
    do {
        if (task->id == id && task != &main_task) {
            if (sig != 0) {
                /* No handler-dispatch mechanism exists (that would mean
                 * injecting a call frame into a running ring-3 task's own
                 * stack and trampolining back — a much larger feature);
                 * every signal takes its POSIX *default* action instead.
                 * exit_code < 0 means "killed by signal -exit_code" (never
                 * ambiguous with a real exit code, which is always >= 0 —
                 * see task_exit()/SYS_WAIT in docs/syscalls.md). */
                task->exit_code = -sig;
                task->state = TASK_ZOMBIE;
            }
            /* sig == 0: the POSIX "null signal" — probe whether the pid
             * exists and is killable, without actually sending anything. */
            return 0;
        }
        task = task->next;
    } while (task && task != task_list_head);
    return -1;
}
