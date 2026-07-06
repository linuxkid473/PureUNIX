#include "shell_internal.h"
#include <stdarg.h>
#include <pureunix/config.h>
#include <pureunix/elf.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
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

static int exec_external(shell_context_t *ctx, shell_command_t *cmd, shell_output_t *out)
{
    char path[PUREUNIX_MAX_PATH];
    if (strchr(cmd->argv[0], '/')) {
        vfs_normalize(path, ctx->cwd, cmd->argv[0]);
        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {
            shell_out_printf(out, "%s: command not found\n", cmd->argv[0]);
            return -1;
        }
        return elf_exec_argv(path, cmd->argc, cmd->argv);
    }

    if (strcmp(cmd->argv[0], "calculator") == 0) {
        snprintf(path, sizeof(path), "/bin/calc.elf");
        vfs_stat_t st;
        if (vfs_stat(path, &st) == 0) {
            return elf_exec_argv(path, cmd->argc, cmd->argv);
        }
    }

    snprintf(path, sizeof(path), "/bin/%s.elf", cmd->argv[0]);
    vfs_stat_t st;
    if (vfs_stat(path, &st) == 0 && elf_exec_argv(path, cmd->argc, cmd->argv) == 0) {
        return 0;
    }
    snprintf(path, sizeof(path), "/bin/%s", cmd->argv[0]);
    if (vfs_stat(path, &st) == 0 && elf_exec_argv(path, cmd->argc, cmd->argv) == 0) {
        return 0;
    }
    shell_out_printf(out, "%s: command not found\n", cmd->argv[0]);
    return -1;
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

        status = run_command(&shell_ctx, cmd, input, &out);
        input = out.buffer;

        if (final) {
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
