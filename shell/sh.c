#include "shell_internal.h"
#include <stdarg.h>
#include <pureunix/config.h>
#include <pureunix/elf.h>
#include <pureunix/errno.h>
#include <pureunix/fcntl.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

static shell_context_t shell_ctx = { .cwd = "/" };
static char stage_a[SHELL_OUTPUT_CAP];
static char stage_b[SHELL_OUTPUT_CAP];

void shell_out_write(shell_output_t *out, const char *data, size_t len)
{
    if (!out || !data) {
        return;
    }
    if (out->terminal) {
        vga_write_len(data, len);
        out->length += len;
        return;
    }
    size_t room = out->capacity > out->length ? out->capacity - out->length - 1 : 0;
    size_t chunk = len < room ? len : room;
    if (chunk) {
        memcpy(out->buffer + out->length, data, chunk);
        out->length += chunk;
        out->buffer[out->length] = '\0';
    }
}

void shell_out_puts(shell_output_t *out, const char *str)
{
    shell_out_write(out, str, strlen(str));
}

void shell_out_printf(shell_output_t *out, const char *fmt, ...)
{
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    shell_out_puts(out, tmp);
}

/* Real fd-based stdout redirection for an external program — unlike a
 * builtin (which never touches a real fd; its output redirection goes
 * through shell_output_t instead, in shell_execute_line() below), a
 * launched ELF program's writes go through its own SYS_WRITE(1, ...),
 * which shell_output_t knows nothing about. There is no separate task for
 * the interactive shell (it runs as task_current(), the same task that
 * calls kernel_main()), so redirecting *this* task's own fd 1 before
 * elf_exec_argv() is what makes the launched program inherit that
 * binding — task_create_user() now shares fds with its creator exactly
 * like task_fork() does (see kernel/task.c) — the same effect a real
 * shell gets from fork()+dup2()+exec(). shell_restore_stdout() undoes it
 * afterward, which is also what actually flushes the write: the shell's
 * own fd 1 was the last reference once the launched program's own copy
 * closed at its exit (see kernel/task.c's close_all_fds()). */
static int shell_redirect_stdout(shell_context_t *ctx, const char *target, bool append)
{
    char path[PUREUNIX_MAX_PATH];
    vfs_normalize(path, ctx->cwd, target);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        int cr = vfs_create(path);
        if (cr != 0 && cr != -EEXIST) {
            return cr;
        }
    } else if (st.type != VFS_FILE) {
        return -EISDIR;
    }

    open_file_t *f = open_file_alloc(FD_KIND_FILE);
    if (!f) {
        return -ENOSPC;
    }
    f->flags = O_WRONLY;
    strncpy(f->path, path, PUREUNIX_MAX_PATH - 1);
    f->path[PUREUNIX_MAX_PATH - 1] = '\0';
    if (append) {
        vfs_read_file(path, &f->data, &f->size); /* best-effort; empty is fine */
        f->offset = f->size;
    }

    task_t *t = task_current();
    open_file_unref(t->fds[1].file); /* normally NULL (console binding) — a no-op */
    t->fds[1].used = true;
    t->fds[1].file = f;
    return 0;
}

static void shell_restore_stdout(void)
{
    task_t *t = task_current();
    open_file_unref(t->fds[1].file);
    t->fds[1].file = NULL;
}

/* The actual path resolution + launch, wrapped by exec_external() below
 * with redirect setup/teardown so every existing return point here (there
 * are several, one per resolution attempt) doesn't need to know about
 * redirection at all. */
static int exec_external_inner(shell_context_t *ctx, shell_command_t *cmd, shell_output_t *out)
{
    char path[PUREUNIX_MAX_PATH];
    if (strchr(cmd->argv[0], '/')) {
        vfs_normalize(path, ctx->cwd, cmd->argv[0]);
        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {
            shell_out_printf(out, "%s: command not found\n", cmd->argv[0]);
            return -1;
        }
        return elf_exec_argv(path, cmd->argc, cmd->argv, shell_build_envp());
    }

    if (strcmp(cmd->argv[0], "calculator") == 0) {
        snprintf(path, sizeof(path), "/bin/calc.elf");
        vfs_stat_t st;
        if (vfs_stat(path, &st) == 0) {
            return elf_exec_argv(path, cmd->argc, cmd->argv, shell_build_envp());
        }
    }

    snprintf(path, sizeof(path), "/bin/%s.elf", cmd->argv[0]);
    vfs_stat_t st;
    if (vfs_stat(path, &st) == 0 && elf_exec_argv(path, cmd->argc, cmd->argv, shell_build_envp()) == 0) {
        return 0;
    }
    snprintf(path, sizeof(path), "/bin/%s", cmd->argv[0]);
    if (vfs_stat(path, &st) == 0 && elf_exec_argv(path, cmd->argc, cmd->argv, shell_build_envp()) == 0) {
        return 0;
    }
    shell_out_printf(out, "%s: command not found\n", cmd->argv[0]);
    return -1;
}

static int exec_external(shell_context_t *ctx, shell_command_t *cmd, shell_output_t *out)
{
    if (!cmd->output[0]) {
        return exec_external_inner(ctx, cmd, out);
    }
    if (shell_redirect_stdout(ctx, cmd->output, cmd->append) != 0) {
        shell_out_printf(out, "%s: cannot open for writing\n", cmd->output);
        return -1;
    }
    int status = exec_external_inner(ctx, cmd, out);
    shell_restore_stdout();
    return status;
}

static int run_command(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (cmd->argc == 0) {
        return 0;
    }
    const builtin_t *builtin = shell_find_builtin(cmd->argv[0]);
    if (builtin) {
        return builtin->fn(ctx, cmd, input, out);
    }
    return exec_external(ctx, cmd, out);
}

static int read_input_redirection(shell_context_t *ctx, const char *path_arg, char **out_text)
{
    char path[PUREUNIX_MAX_PATH];
    uint8_t *data;
    size_t size;
    vfs_normalize(path, ctx->cwd, path_arg);
    if (vfs_read_file(path, &data, &size) != 0) {
        return -1;
    }
    char *text = kmalloc(size + 1);
    if (!text) {
        kfree(data);
        return -1;
    }
    memcpy(text, data, size);
    text[size] = '\0';
    kfree(data);
    *out_text = text;
    return 0;
}

int shell_execute_line(const char *line)
{
    shell_pipeline_t pipeline;
    shell_output_t terminal = { .terminal = true };
    if (shell_parse(line, &pipeline, &terminal) != 0) {
        return -1;
    }
    if (pipeline.commands[0].argc == 0) {
        return 0;
    }

    const char *input = NULL;
    char *redir_input = NULL;
    int status = 0;

    for (int i = 0; i < pipeline.count; ++i) {
        shell_command_t *cmd = &pipeline.commands[i];
        bool final = i == pipeline.count - 1;
        char *buf = (i % 2) ? stage_b : stage_a;
        memset(buf, 0, SHELL_OUTPUT_CAP);
        shell_output_t out = {
            .buffer = buf,
            .capacity = SHELL_OUTPUT_CAP,
            .length = 0,
            .terminal = false,
        };

        if (cmd->input[0]) {
            if (redir_input) {
                kfree(redir_input);
                redir_input = NULL;
            }
            if (read_input_redirection(&shell_ctx, cmd->input, &redir_input) != 0) {
                printf("%s: cannot read input\n", cmd->input);
                return -1;
            }
            input = redir_input;
        }

        /* A builtin's output only ever exists in out.buffer (it's built by
         * explicit shell_out_*() calls — see shell/builtins.c), so
         * redirecting/printing it here is the only place that can happen.
         * An external ELF program's own output goes through its own real
         * fd 1 instead (out.buffer stays empty for it) — exec_external()
         * already redirected that fd before launching it if cmd->output
         * was set (see shell_redirect_stdout() above), and its normal,
         * unredirected output already reached the console directly via
         * SYS_WRITE, so there's nothing left to do here for it either
         * way. */
        bool is_builtin = shell_find_builtin(cmd->argv[0]) != NULL;
        status = run_command(&shell_ctx, cmd, input, &out);
        input = out.buffer;

        if (final && is_builtin) {
            if (cmd->output[0]) {
                char path[PUREUNIX_MAX_PATH];
                vfs_normalize(path, shell_ctx.cwd, cmd->output);
                if (vfs_write_file(path, (const uint8_t *)out.buffer, out.length,
                                   cmd->append ? VFS_O_APPEND : VFS_O_TRUNC) != 0) {
                    printf("%s: cannot write output\n", path);
                    status = -1;
                }
            } else {
                vga_write_len(out.buffer, out.length);
            }
        }
    }

    if (redir_input) {
        kfree(redir_input);
    }
    return status;
}

void shell_set_home_cwd(const char *home)
{
    if (!home || !*home) {
        return;
    }
    vfs_stat_t st;
    if (vfs_stat(home, &st) == 0 && st.type == VFS_DIR) {
        strncpy(shell_ctx.cwd, home, sizeof(shell_ctx.cwd) - 1);
        shell_ctx.cwd[sizeof(shell_ctx.cwd) - 1] = '\0';
    }
}

void shell_run(void)
{
    char line[256];
    printf("\nWelcome to PureUnix. Type 'help' for commands.\n");
    for (;;) {
        char prompt[PUREUNIX_MAX_PATH + 8];
        snprintf(prompt, sizeof(prompt), "%s %s", shell_ctx.cwd, PUREUNIX_PROMPT);
        shell_readline(prompt, line, sizeof(line));
        shell_execute_line(line);
    }
}
