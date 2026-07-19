#include <pureunix/arch.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/flock.h>
#include <pureunix/memory.h>
#include <pureunix/pty.h>
#include <pureunix/signal.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/time.h>
#include <pureunix/unix_socket.h>
#include <pureunix/vfs.h>
#include <pureunix/vmm.h>
#include <pureunix/vt.h>
#include <pureunix/wait.h>

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
/* The init-reaper task's pid (kernel/main.c spawns it at boot) —
 * task_exit() re-parents every orphaned child to this task, which does
 * nothing but loop task_waitpid(-1, NULL) forever, guaranteeing no
 * process is ever a permanent zombie just because its real parent exited
 * first. 0 (never a valid pid) until kernel/main.c calls
 * task_set_init_pid(), which is early enough in boot that no real
 * process can exit before it's set. */
static uint32_t g_init_pid;

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
     * task is kcalloc'd, so fds[0..2].file/.closed_explicitly already start
     * NULL/false — the default console binding, not (yet) reclaimable by a
     * fresh allocation (see include/pureunix/task.h's fd_entry_t comment)
     * — with nothing further to set up here. */
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
        /* Real POSIX drops every advisory lock a process holds on a file
         * when any fd referring to it is closed; here "owner" is this
         * open_file_t itself (include/pureunix/flock.h explains why that's
         * the right identity), so releasing on its own teardown is exact,
         * not an approximation. */
        flock_release_owner(f);
    } else if (f->kind == FD_KIND_PIPE) {
        pipe_buf_t *p = f->pipe_buf;
        if (p) {
            if (f->pipe_is_write_end) {
                p->write_ends--;
                /* Last writer gone: any reader blocked on an empty pipe
                 * must wake up to see EOF instead of waiting forever. */
                if (p->write_ends == 0) {
                    wait_queue_wake_all(&p->read_wq);
                }
            } else {
                p->read_ends--;
                /* Last reader gone: any writer blocked on a full pipe
                 * must wake up to see EPIPE instead of waiting forever. */
                if (p->read_ends == 0) {
                    wait_queue_wake_all(&p->write_wq);
                }
            }
            if (p->read_ends == 0 && p->write_ends == 0) {
                kfree(p);
            }
        }
    } else if (f->kind == FD_KIND_SOCKET) {
        /* Real AF_UNIX socket end (include/pureunix/unix_socket.h) —
         * unix_socket_unref() itself handles waking a connected peer's
         * blocked reader/writer (EOF/EPIPE) and reclaiming the pool
         * slot, exactly like pty_master_unref()/pty_slave_unref() just
         * below. */
        unix_socket_unref(f->usock);
    } else if (f->kind == FD_KIND_PTY) {
        /* Real PTY pair (include/pureunix/pty.h) — the pty_t itself lives
         * in kernel/pty.c's own fixed pool (not kmalloc'd), so there's
         * nothing to kfree() here; pty_master_unref()/pty_slave_unref()
         * handle waking the opposite end's blocked reader/writer and
         * reclaiming the pool slot once both ends are gone, exactly like
         * the pipe_buf_t case above. */
        if (f->pty_is_master) {
            pty_master_unref(f->pty);
        } else {
            pty_slave_unref(f->pty);
        }
    }
    /* FD_KIND_TTY: a /dev/ttyN descriptor owns no buffer of its own (reads/
     * writes go straight through to kernel/vt.c) — nothing to flush or
     * retire on close, matching real UNIX close() on a tty device node. */
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
    main_task.vt_id = -1; /* claimed as VT1 once kernel_main() calls vt_init() */
    strcpy(main_task.cwd, "/");
    /* The boot task has no parent; it's its own process group and session
     * leader by definition (there's nothing else running yet for it to
     * inherit either from). */
    main_task.ppid = 0;
    main_task.pgid = main_task.id;
    main_task.sid = main_task.id;
    main_task.nice = 0;
    main_task.start_time = time_now();
    strncpy(main_task.cmdline, main_task.name, sizeof(main_task.cmdline) - 1);
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
    task->state = TASK_RUNNABLE;
    reserve_stdio(task);
    /* Credentials propagate from creator to child — the only "process
     * spawning" rule that exists before a real login/setuid model arrives. */
    task->uid = current ? current->uid : 0;
    task->gid = current ? current->gid : 0;
    task->vt_id = current ? current->vt_id : -1;
    /* A controlling-terminal pty (include/pureunix/pty.h) is inherited
     * across fork()/exec() exactly like vt_id above — real POSIX ctty
     * semantics (only setsid() or an explicit TIOCSCTTY changes it). NULL
     * (the kcalloc() default) for every task whose creator has none, which
     * is every task except PUTerm's own forked child (docs/pude.md). */
    task->ctty_pty = current ? current->ctty_pty : NULL;
    strcpy(task->cwd, current && current->cwd[0] ? current->cwd : "/");
    task->ppid = current ? current->id : 0;
    /* A fresh task starts in its creator's process group/session — real
     * group/session changes only ever happen via an explicit setpgid()/
     * setsid() call afterwards (see kernel/task.c's task_setpgid()). */
    task->pgid = current ? current->pgid : task->id;
    task->sid = current ? current->sid : task->id;
    task->nice = current ? current->nice : 0;
    task->start_time = time_now();
    strncpy(task->cmdline, task->name, sizeof(task->cmdline) - 1);
    task->pd_phys = current ? current->pd_phys : vmm_kernel_directory_phys();

    /* See task_t.fpu_state's own comment (include/pureunix/task.h) --
     * this task's own FXSAVE image must already hold a legal "freshly
     * reset FPU" state before it can ever be FXRSTOR'd as `next` in
     * task_yield() below, which is the very first time this buffer would
     * otherwise be touched. */
    fpu_init_task_state(task->fpu_state);

    uint32_t *sp = (uint32_t *)(task->stack_base + TASK_STACK_SIZE);
    *--sp = (uint32_t)task_bootstrap;
    *--sp = 0x202;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    task->stack_ptr = sp;

    /* task->next is set *before* splicing the new node into the list
     * (not after) so an IRQ-context list walk (kernel/vt.c's
     * vt_input_push() -> kernel/signal.c's signal_send_pgrp() ->
     * task_list(), M6) can never observe a task whose ->next is still
     * NULL (kcalloc's zero-fill) — the single `tail->next = task`
     * pointer write below is what actually makes the new node visible,
     * and by then it's already fully formed. */
    task_t *tail = task_list_head;
    while (tail->next != task_list_head) {
        tail = tail->next;
    }
    task->next = task_list_head;
    tail->next = task;
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
            task->fds[i].closed_explicitly = current->fds[i].closed_explicitly;
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
    /* vmm_fork_address_space() deep-copies the whole address space, TLS
     * block included, so the same virtual address is still valid (and
     * still holds an independent copy) in the child — see task_t.tls_base's
     * own comment (include/pureunix/task.h). */
    child->tls_base = current->tls_base;
    /* vmm_fork_address_space() just copied the parent's whole address
     * space, so the child's real mapped size starts out identical —
     * see include/pureunix/task.h's mapped_bytes comment. */
    child->mapped_bytes = current->mapped_bytes;
    /* vmm_fork_address_space() already deep-copied FB_SHADOW_VA's mapping
     * too (it copies the whole user window, not just code/data/stack) —
     * this flag just has to agree, or a later SYS_FB_MMAP call from the
     * child would allocate fresh frames and overwrite that already-valid
     * copy instead of reusing it (see include/pureunix/task.h's comment). */
    child->fb_shadow_mapped = current->fb_shadow_mapped;
    /* Same reasoning: vmm_fork_address_space() already deep-copied every
     * heap page too (it copies the whole user window), this flag just has
     * to agree so the child's own later sbrk() growth continues from the
     * right offset instead of re-mapping over the parent's copied pages. */
    child->heap_used = current->heap_used;
    child->heap_mapped = current->heap_mapped;
    child->heap_base = current->heap_base;
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
        child->fds[i].closed_explicitly = current->fds[i].closed_explicitly;
        child->fds[i].cloexec = current->fds[i].cloexec;
        child->fds[i].file = current->fds[i].file;
        open_file_ref(child->fds[i].file);
    }

    /* Signal dispositions and the blocked-signal mask are inherited
     * as-is across fork() — POSIX. Critically, this is what makes an
     * interactive shell's own SIGINT/SIGQUIT self-protection (installing
     * SIG_IGN for itself, standard practice for any POSIX shell so
     * Ctrl+C on a foreground *job* doesn't also kill the shell, since a
     * freshly forked job shares the shell's own process group) actually
     * take effect in the child too — a real handler function pointer is
     * also inherited here (matches POSIX fork()) but is meaningless
     * until the child calls exec(), at which point elf_exec_current()
     * resets it back to SIG_DFL (the code it pointed to no longer exists
     * once the image is replaced) while preserving SIG_IGN, exactly like
     * a real exec(). Pending signals are deliberately NOT inherited — a
     * fresh child starts with none of its own, matching POSIX. */
    memcpy(child->sig_handlers, current->sig_handlers, sizeof(child->sig_handlers));
    child->blocked_signals = current->blocked_signals;

    /* task_alloc() already defaulted child->cmdline to just its generic
     * task name — a fork()ed child should instead start out reporting
     * the *same* cmdline as its parent (real fork() semantics: argv isn't
     * touched until the child calls its own exec(), see kernel/elf.c's
     * pack_cmdline()). */
    strncpy(child->cmdline, current->cmdline, sizeof(child->cmdline) - 1);
    child->cmdline[sizeof(child->cmdline) - 1] = '\0';

    return child;
}

/* nice-weighted round robin: a candidate with nice > 0 is skipped
 * `nice` extra scheduling passes between turns (skip_counter counts
 * those down), so a heavily-reniced task measurably gets less of the
 * CPU than its neighbors — a real, observable scheduling difference,
 * not cosmetic bookkeeping. Deliberately doesn't implement the
 * opposite (a negative-nice task getting picked *more* often than a
 * plain round-robin turn) — that needs real accounting (a vruntime-
 * style scheme) this cooperative scheduler has no other use for; the
 * de-prioritization side alone is what "nice"/"renice" are almost
 * always actually used to demonstrate or rely on in practice, and it's
 * already sufficient to make a low-priority vs. default/high-priority
 * comparison come out correctly. See docs/process-management.md. */
static task_t *next_ready_task(void)
{
    task_t *candidate = current->next;
    while (candidate != current) {
        if (candidate->state == TASK_RUNNABLE || candidate->state == TASK_RUNNING) {
            if (candidate->nice > 0 && candidate->skip_counter > 0) {
                candidate->skip_counter--;
                candidate = candidate->next;
                continue;
            }
            if (candidate->nice > 0) {
                candidate->skip_counter = candidate->nice;
            }
            return candidate;
        }
        candidate = candidate->next;
    }
    return current;
}

bool task_other_runnable_exists(void)
{
    if (!current) {
        return false;
    }
    task_t *candidate = current->next;
    while (candidate != current) {
        if (candidate->state == TASK_RUNNABLE) {
            return true;
        }
        candidate = candidate->next;
    }
    return false;
}

void task_yield(void)
{
    task_t *prev = current;
    task_t *next = next_ready_task();
    if (next == prev) {
        return;
    }
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_RUNNABLE;
    }
    next->state = TASK_RUNNING;
    /* Fresh quantum for whichever task is about to actually run — see
     * arch/i386/pit.c's minimal-preemption check. */
    next->quantum_ticks = 0;
    current = next;
    /* main_task runs on the boot stack (no stack_base) and never executes
     * in ring 3, so it has nothing to install here. */
    if (next->stack_base) {
        tss_set_kernel_stack((uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }
    if (next->pd_phys != prev->pd_phys) {
        vmm_switch_directory(next->pd_phys);
    }
    /* See task_t.tls_base's own comment (include/pureunix/task.h): the
     * CPU has no per-task notion of %gs's base address on its own, so the
     * shared TLS descriptor has to be repointed at whichever task is
     * about to actually run, every switch — same reasoning as
     * tss_set_kernel_stack()/vmm_switch_directory() just above. */
    gdt_set_tls_base(next->tls_base);
    /* Real x87/SSE register file save/restore -- see task_t.fpu_state's
     * own comment (include/pureunix/task.h). Must happen before
     * context_switch()'s stack-pointer swap: the CPU's actual FPU/SSE
     * registers are global machine state, entirely untouched by that
     * swap (it only pushes/pops GP registers + eflags), so `next` must
     * already have its own state loaded by the time execution actually
     * resumes at whatever eip its stack holds. */
    fpu_save(fpu_state_area(prev->fpu_state));
    fpu_restore(fpu_state_area(next->fpu_state));
    context_switch(&prev->stack_ptr, next->stack_ptr);
}

void task_set_init_pid(uint32_t pid)
{
    g_init_pid = pid;
}

void task_exit(int code)
{
    current->exit_code = code;
    current->state = TASK_ZOMBIE;

    /* Belt-and-suspenders with kernel/signal.c's identical cleanup on the
     * signal-killed path: a task that put its own VT into graphics mode
     * but exits some other way (_exit() bypassing its own atexit/SDL
     * teardown, a bug, ...) must not leave that VT's console permanently
     * suppressed and its ASCII input queue permanently dropping every
     * keystroke — see kernel/vt.c's vt_input_push() graphics_mode gate.
     *
     * Must also check *this task is the one that turned graphics mode on*
     * (vt_get_graphics_owner()), not just "shares this vt_id" — every
     * child PUTerm forks (a shell, and everything *it* forks+execs, e.g.
     * `ls`) inherits the same task_t.vt_id its ancestor had, exactly like
     * a real shell's children inherit their controlling terminal. Before
     * this check existed, an ordinary, successful `ls` exiting normally
     * forced the whole VT out of graphics mode out from under PUTerm,
     * which was still very much alive and running — confirmed as the
     * actual, reproducible cause of PUTerm's window vanishing back to the
     * plain text console the instant any real external (fork+exec'd, not
     * an ash builtin) command finished (docs/pude.md). */
    if (current->vt_id >= 0 && vt_is_graphics_mode(current->vt_id) &&
        vt_get_graphics_owner(current->vt_id) == current->id) {
        vt_set_graphics_mode(current->vt_id, false);
    }

    /* Re-parent every child of the exiting task to the init-reaper
     * *before* signaling anyone or yielding away, so no orphan is ever
     * left with a ppid pointing at a task_t that's about to be reaped out
     * from under it — see g_init_pid's comment above. */
    if (g_init_pid != 0) {
        task_t *t = task_list_head;
        do {
            if (t->ppid == current->id) {
                t->ppid = g_init_pid;
            }
            t = t->next;
        } while (t != task_list_head);
    }

    task_t *parent = task_find(current->ppid);
    signal_send(parent, SIGCHLD);
    /* Wake a parent genuinely blocked in task_waitpid()'s wait_queue_sleep()
     * (see task_t.child_wait's own comment) — signal_send() above only
     * ever sets a pending-signal bit or, for a real SIGCHLD handler,
     * defers to the ring3-return boundary; neither actually reschedules
     * a parent that's parked here waiting for exactly this transition. */
    if (parent) {
        wait_queue_wake_all(&parent->child_wait);
    }

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

/* exec()'s close-on-exec pass (kernel/elf.c's elf_exec_current(), the only
 * exec() path that keeps the calling task's own fd table — elf_exec_argv()
 * always creates a brand-new task_t with a fresh, empty one instead, so
 * close-on-exec is meaningless there). Only ever closes a real fd (>= 3
 * with a real open_file_t); fd_entry_t.cloexec is only ever set true by
 * fcntl(F_DUPFD_CLOEXEC) (arch/i386/syscall.c), which never targets a
 * console-bound 0/1/2 slot in practice (find_free_fd() always finds a real
 * fd >= 3 first), but the fd>=3 guard here keeps that assumption from
 * ever silently reverting 0/1/2 to the console binding if it somehow did. */
void task_close_cloexec_fds(task_t *t)
{
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (t->fds[i].used && t->fds[i].cloexec) {
            open_file_unref(t->fds[i].file);
            t->fds[i].file = NULL;
            t->fds[i].used = false;
            t->fds[i].cloexec = false;
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

typedef struct {
    task_t *parent;
    int pid;
    int options;
} waitpid_ctx_t;

/* True the instant *some* matching child has a state task_waitpid() would
 * act on — never mutates anything (unlike task_waitpid()'s own scan,
 * which reaps/marks as a side effect), so it's safe to call repeatedly
 * from wait_queue_sleep()'s check-and-block loop. */
static bool waitpid_predicate(void *ctx_)
{
    waitpid_ctx_t *ctx = ctx_;
    task_t *t = task_list_head;
    if (!t) {
        return false;
    }
    do {
        if (t->ppid == ctx->parent->id && (ctx->pid == -1 || (int)t->id == ctx->pid)) {
            if (t->state == TASK_ZOMBIE) {
                return true;
            }
            if ((ctx->options & PU_WUNTRACED) && t->state == TASK_STOPPED && !t->stop_reported) {
                return true;
            }
        }
        t = t->next;
    } while (t != task_list_head);
    return false;
}

int task_waitpid(int pid, int *status, int options)
{
    for (;;) {
        bool have_child = false;
        task_t *t = task_list_head;
        do {
            if (t->ppid == current->id && (pid == -1 || (int)t->id == pid)) {
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
                /* See task_t.stop_signal/stop_reported (task.h) and
                 * kernel/signal.c's transitions into/out of TASK_STOPPED —
                 * this sentinel (raw_code <= -1000) is disjoint from both
                 * the normal-exit (>=0) and killed-by-signal (-127..-1)
                 * ranges task_waitpid() already returns, so callers can
                 * always tell the three apart. See docs/syscalls.md. */
                if ((options & PU_WUNTRACED) && t->state == TASK_STOPPED &&
                    !t->stop_reported) {
                    t->stop_reported = true;
                    if (status) {
                        *status = -1000 - t->stop_signal;
                    }
                    return (int)t->id;
                }
            }
            t = t->next;
        } while (t != task_list_head);

        if (!have_child) {
            return -1;
        }
        if (options & PU_WNOHANG) {
            /* Real POSIX WNOHANG semantics: a matching child exists but
             * hasn't changed state yet -- return 0 (never a valid pid,
             * since ids start at 1) instead of blocking. A caller that
             * needs to poll for a child's exit without stalling its own
             * event loop (PUTerm's WM, docs/pude.md -- it must keep
             * servicing its own render/input loop regardless of whether
             * the shell it forked has exited yet) needs exactly this; it
             * was accepted-but-not-implemented until this was the first
             * real caller (include/pureunix/task.h's PU_WNOHANG comment).
             * user/newlib_syscalls.c's waitpid() already passes an rc of 0
             * straight through unmodified — no translation needed there. */
            return 0;
        }
        /* Real block on current->child_wait — see that field's own
         * comment for why the old task_yield()-only loop this replaced
         * was a real, user-visible bug (a busy-spin at 100% CPU for the
         * entire lifetime of every foreground job), not just style.
         * Woken by task_exit() and signal_send()'s TASK_STOPPED/
         * TASK_ZOMBIE-producing transitions. */
        waitpid_ctx_t ctx = { .parent = current, .pid = pid, .options = options };
        wait_queue_sleep(&current->child_wait, waitpid_predicate, &ctx);
    }
}

task_t *task_current(void)
{
    return current;
}

task_t *task_find(uint32_t id)
{
    if (!task_list_head) {
        return NULL;
    }
    task_t *t = task_list_head;
    do {
        if (t->id == id) {
            return t;
        }
        t = t->next;
    } while (t && t != task_list_head);
    return NULL;
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

int task_setpgid(uint32_t pid, uint32_t pgid)
{
    task_t *target = pid == 0 ? current : task_find(pid);
    if (!target) {
        return -ESRCH;
    }
    if (target != current && target->ppid != current->id) {
        return -EPERM;
    }
    /* A session leader's own group can never be changed — it would leave
     * the group it currently leads (which may still have other members)
     * leaderless while conflating its identity with a different group. */
    if (target->pgid == target->sid && target->id == target->sid) {
        return -EPERM;
    }
    uint32_t new_pgid = pgid == 0 ? target->id : pgid;
    /* setpgid() only ever moves a process within its own session — never
     * across sessions (POSIX). */
    target->pgid = new_pgid;
    return 0;
}

int task_getpgid(uint32_t pid)
{
    task_t *target = pid == 0 ? current : task_find(pid);
    if (!target) {
        return -ESRCH;
    }
    return (int)target->pgid;
}

int task_setsid(void)
{
    if (current->pgid == current->id) {
        /* Already a process group leader — POSIX forbids setsid() here
         * (see include/pureunix/task.h). */
        return -EPERM;
    }
    current->sid = current->id;
    current->pgid = current->id;
    return (int)current->id;
}

int task_getsid(uint32_t pid)
{
    task_t *target = pid == 0 ? current : task_find(pid);
    if (!target) {
        return -ESRCH;
    }
    return (int)target->sid;
}

int task_setpriority(uint32_t pid, int nice)
{
    task_t *target = pid == 0 ? current : task_find(pid);
    if (!target) {
        return -ESRCH;
    }
    if (nice < -20) {
        nice = -20;
    } else if (nice > 19) {
        nice = 19;
    }
    /* No privilege check — every task in this kernel effectively runs as
     * root today (see include/pureunix/task.h's uid/gid comment), so
     * there's no real distinction to enforce yet between "lower my own
     * niceness" (POSIX: requires privilege) and any other renice. */
    target->nice = nice;
    /* Takes effect starting the *next* time this task is considered, not
     * mid-cooldown from whatever its old nice value had already counted
     * down. */
    target->skip_counter = 0;
    return 0;
}

int task_getpriority(uint32_t pid, int *out_nice)
{
    task_t *target = pid == 0 ? current : task_find(pid);
    if (!target) {
        return -ESRCH;
    }
    if (out_nice) {
        *out_nice = target->nice;
    }
    return 0;
}

int task_kill(uint32_t id, int sig)
{
    task_t *target = task_find(id);
    if (!target || target == &main_task) {
        return -1;
    }
    if (sig != 0) {
        /* Real signal delivery — default action, SIG_IGN, or a real
         * handler (kernel/signal.c). exit_code < 0 (when a default action
         * terminates the target) means "killed by signal -exit_code",
         * never ambiguous with a real exit code (always >= 0) — see
         * task_exit()/SYS_WAIT in docs/syscalls.md. */
        signal_send(target, sig);
    }
    /* sig == 0: the POSIX "null signal" — probe whether the pid exists
     * and is killable, without actually sending anything. */
    return 0;
}

bool task_pgrp_exists(uint32_t pgid)
{
    if (!task_list_head) {
        return false;
    }
    task_t *t = task_list_head;
    do {
        if (t->pgid == pgid && t->state != TASK_ZOMBIE) {
            return true;
        }
        t = t->next;
    } while (t != task_list_head);
    return false;
}

void task_yield_if_not_running(task_t *t)
{
    if (!t || t->state == TASK_RUNNING) {
        return;
    }
    task_yield();
    if (t->state == TASK_ZOMBIE) {
        /* Never let a self-killed task's own code path resume executing
         * real logic if task_yield() found nothing else ready and just
         * no-op'd — mirrors task_exit()'s own halt-loop for that exact
         * situation. */
        for (;;) {
            arch_halt();
        }
    }
}
