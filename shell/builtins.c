#include "shell_internal.h"
#include <pureunix/arch.h>
#include <pureunix/ctype.h>
#include <pureunix/editor.h>
#include <pureunix/elf.h>
#include <pureunix/fat16.h>
#include <pureunix/io.h>
#include <pureunix/kernel.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/stdlib.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

typedef struct env_var {
    char key[32];
    char value[96];
} env_var_t;

static env_var_t env[32] = {
    { "USER", "root" },
    { "HOME", "/" },
    { "PATH", "/bin" },
    { "SHELL", "sh" },
};
static size_t env_count = 4;

static void abs_path(shell_context_t *ctx, const char *path, char *out)
{
    vfs_normalize(out, ctx->cwd, path);
}

const char *shell_getenv(const char *key)
{
    for (size_t i = 0; i < env_count; ++i) {
        if (strcmp(env[i].key, key) == 0) {
            return env[i].value;
        }
    }
    return "";
}

int shell_setenv(const char *key, const char *value)
{
    if (!key || !*key || strchr(key, '=')) {
        return -1;
    }
    for (size_t i = 0; i < env_count; ++i) {
        if (strcmp(env[i].key, key) == 0) {
            strncpy(env[i].value, value ? value : "", sizeof(env[i].value) - 1);
            return 0;
        }
    }
    if (env_count >= ARRAY_SIZE(env)) {
        return -1;
    }
    strncpy(env[env_count].key, key, sizeof(env[env_count].key) - 1);
    strncpy(env[env_count].value, value ? value : "", sizeof(env[env_count].value) - 1);
    env_count++;
    return 0;
}

void shell_env_list(shell_output_t *out)
{
    for (size_t i = 0; i < env_count; ++i) {
        shell_out_printf(out, "%s=%s\n", env[i].key, env[i].value);
    }
}

static int cmd_help(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_ls(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_cd(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_pwd(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_mkdir(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_rmdir(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_rm(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_cp(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_mv(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_cat(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_echo(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_touch(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_clear(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_history(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_date(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_uname(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_whoami(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_mount(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_df(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_free(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_ps(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_kill(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_reboot(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_shutdown(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_env(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_export(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);
static int cmd_editor(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);

static const builtin_t builtins[] = {
    { "ls", "list directory contents", cmd_ls },
    { "cd", "change directory", cmd_cd },
    { "pwd", "print working directory", cmd_pwd },
    { "mkdir", "create a directory", cmd_mkdir },
    { "rmdir", "remove an empty directory", cmd_rmdir },
    { "rm", "remove a file", cmd_rm },
    { "cp", "copy a file", cmd_cp },
    { "mv", "move or rename a file", cmd_mv },
    { "cat", "print files or stdin", cmd_cat },
    { "echo", "print text", cmd_echo },
    { "touch", "create an empty file", cmd_touch },
    { "clear", "clear the screen", cmd_clear },
    { "history", "show command history", cmd_history },
    { "date", "show CMOS date/time", cmd_date },
    { "uname", "show system name", cmd_uname },
    { "whoami", "show current user", cmd_whoami },
    { "mount", "show mounted filesystems", cmd_mount },
    { "df", "show filesystem space", cmd_df },
    { "free", "show memory usage", cmd_free },
    { "ps", "show tasks", cmd_ps },
    { "kill", "mark a task as stopped", cmd_kill },
    { "reboot", "restart the machine", cmd_reboot },
    { "shutdown", "power off the machine", cmd_shutdown },
    { "help", "show built-in commands", cmd_help },
    { "env", "show environment variables", cmd_env },
    { "export", "set environment variable", cmd_export },
    { "vim", "open the vim text editor", cmd_editor },
    { "vi",  "open the vim text editor", cmd_editor },
};

const builtin_t *shell_builtins(size_t *count)
{
    if (count) {
        *count = ARRAY_SIZE(builtins);
    }
    return builtins;
}

const builtin_t *shell_find_builtin(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(builtins); ++i) {
        if (strcmp(builtins[i].name, name) == 0) {
            return &builtins[i];
        }
    }
    return NULL;
}

static int require_args(shell_command_t *cmd, int n, shell_output_t *out, const char *usage)
{
    if (cmd->argc < n) {
        shell_out_printf(out, "usage: %s\n", usage);
        return -1;
    }
    return 0;
}

static int cmd_help(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    for (size_t i = 0; i < ARRAY_SIZE(builtins); ++i) {
        shell_out_printf(out, "%s - %s\n", builtins[i].name, builtins[i].summary);
    }
    shell_out_puts(out, "Pipes and redirection are supported: |, >, >>, <\n");
    return 0;
}

static int ls_cb(const vfs_dirent_t *entry, void *ctx)
{
    shell_output_t *out = ctx;
    shell_out_printf(out, "%c %s %u\n", entry->type == VFS_DIR ? 'd' : '-', entry->name, entry->size);
    return 0;
}

static int cmd_ls(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argc > 1 ? cmd->argv[1] : ".", path);
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        shell_out_printf(out, "ls: cannot access %s\n", path);
        return -1;
    }
    if (st.type == VFS_FILE) {
        shell_out_printf(out, "- %s %u\n", path, st.size);
        return 0;
    }
    return vfs_readdir(path, ls_cb, out);
}

static int cmd_cd(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argc > 1 ? cmd->argv[1] : shell_getenv("HOME"), path);
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.type != VFS_DIR) {
        shell_out_printf(out, "cd: not a directory: %s\n", path);
        return -1;
    }
    strcpy(ctx->cwd, path);
    return 0;
}

static int cmd_pwd(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_printf(out, "%s\n", ctx->cwd);
    return 0;
}

static int cmd_mkdir(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "mkdir DIR") != 0) return -1;
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], path);
    if (vfs_mkdir(path) != 0) {
        shell_out_printf(out, "mkdir: failed: %s\n", path);
        return -1;
    }
    return 0;
}

static int cmd_rmdir(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "rmdir DIR") != 0) return -1;
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], path);
    if (vfs_rmdir(path) != 0) {
        shell_out_printf(out, "rmdir: failed: %s\n", path);
        return -1;
    }
    return 0;
}

static int cmd_rm(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "rm FILE") != 0) return -1;
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], path);
    if (vfs_unlink(path) != 0) {
        shell_out_printf(out, "rm: failed: %s\n", path);
        return -1;
    }
    return 0;
}

static int cmd_cp(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 3, out, "cp SRC DST") != 0) return -1;
    char src[PUREUNIX_MAX_PATH], dst[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], src);
    abs_path(ctx, cmd->argv[2], dst);
    uint8_t *data;
    size_t size;
    if (vfs_read_file(src, &data, &size) != 0) {
        shell_out_printf(out, "cp: cannot read %s\n", src);
        return -1;
    }
    int r = vfs_write_file(dst, data, size, VFS_O_TRUNC);
    kfree(data);
    if (r != 0) {
        shell_out_printf(out, "cp: cannot write %s\n", dst);
        return -1;
    }
    return 0;
}

static int cmd_mv(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 3, out, "mv SRC DST") != 0) return -1;
    char src[PUREUNIX_MAX_PATH], dst[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], src);
    abs_path(ctx, cmd->argv[2], dst);
    if (vfs_rename(src, dst) == 0) {
        return 0;
    }
    uint8_t *data;
    size_t size;
    if (vfs_read_file(src, &data, &size) != 0) {
        shell_out_printf(out, "mv: cannot read %s\n", src);
        return -1;
    }
    int r = vfs_write_file(dst, data, size, VFS_O_TRUNC);
    kfree(data);
    if (r == 0) {
        vfs_unlink(src);
        return 0;
    }
    shell_out_puts(out, "mv: failed\n");
    return -1;
}

static int cmd_cat(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (cmd->argc == 1) {
        shell_out_puts(out, input ? input : "");
        return 0;
    }
    for (int i = 1; i < cmd->argc; ++i) {
        char path[PUREUNIX_MAX_PATH];
        abs_path(ctx, cmd->argv[i], path);
        uint8_t *data;
        size_t size;
        if (vfs_read_file(path, &data, &size) != 0) {
            shell_out_printf(out, "cat: cannot read %s\n", path);
            return -1;
        }
        shell_out_write(out, (const char *)data, size);
        kfree(data);
    }
    return 0;
}

static void echo_expanded(shell_output_t *out, const char *arg)
{
    if (arg[0] != '$') {
        shell_out_puts(out, arg);
        return;
    }
    shell_out_puts(out, shell_getenv(arg + 1));
}

static int cmd_echo(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    for (int i = 1; i < cmd->argc; ++i) {
        if (i > 1) shell_out_puts(out, " ");
        echo_expanded(out, cmd->argv[i]);
    }
    shell_out_puts(out, "\n");
    return 0;
}

static int cmd_touch(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "touch FILE") != 0) return -1;
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], path);
    if (vfs_create(path) != 0) {
        vfs_write_file(path, (const uint8_t *)"", 0, VFS_O_APPEND);
    }
    return 0;
}

static int cmd_clear(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    vga_clear();
    return 0;
}

static int cmd_history(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    for (size_t i = 0; i < shell_history_count(); ++i) {
        shell_out_printf(out, "%u  %s\n", (uint32_t)i + 1, shell_history_get(i));
    }
    return 0;
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}

static int cmd_date(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    uint8_t status_b = cmos_read(0x0B);
    bool binary = status_b & 0x04;
    int sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    int day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    if (!binary) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
    }
    shell_out_printf(out, "20%02u-%02u-%02u %02u:%02u:%02u UTC\n", year, mon, day, hour, min, sec);
    return 0;
}

static int cmd_uname(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_puts(out, "PureUnix pureunix 0.1.0 i686\n");
    return 0;
}

static int cmd_whoami(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_printf(out, "%s\n", shell_getenv("USER"));
    return 0;
}

static int cmd_mount(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (vfs_mounted()) {
        shell_out_puts(out, "/dev/ata0 on / type fat16 (rw,8.3)\n");
    } else {
        shell_out_puts(out, "no filesystems mounted\n");
    }
    return 0;
}

static int cmd_df(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_puts(out, "Filesystem  1K-blocks  Used  Available  Mounted on\n");
    if (vfs_mounted()) {
        uint32_t total = fat16_total_bytes() / 1024;
        uint32_t freeb = fat16_free_bytes() / 1024;
        shell_out_printf(out, "fat16       %u  %u  %u  /\n", total, total - freeb, freeb);
    }
    return 0;
}

static int cmd_free(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_printf(out, "physical: %u KiB total, %u KiB free\n", pmm_total_memory_kb(), pmm_free_memory_kb());
    shell_out_printf(out, "heap:     %u bytes used, %u bytes free\n", (uint32_t)heap_used_bytes(), (uint32_t)heap_free_bytes());
    return 0;
}

static void ps_cb(const task_t *task, void *ctx)
{
    shell_output_t *out = ctx;
    const char *state = task->state == TASK_RUNNING ? "run" :
                        task->state == TASK_READY ? "ready" :
                        task->state == TASK_SLEEPING ? "sleep" : "zombie";
    shell_out_printf(out, "%u\t%s\t%s\n", task->id, state, task->name);
}

static int cmd_ps(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_out_puts(out, "PID\tSTATE\tNAME\n");
    task_list(ps_cb, out);
    return 0;
}

static int cmd_kill(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "kill PID") != 0) return -1;
    if (task_kill((uint32_t)atoi(cmd->argv[1])) != 0) {
        shell_out_puts(out, "kill: no such task\n");
        return -1;
    }
    return 0;
}

static int cmd_reboot(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    kernel_reboot();
    return 0;
}

static int cmd_shutdown(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    kernel_shutdown();
    return 0;
}

static int cmd_env(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    shell_env_list(out);
    return 0;
}

static int cmd_export(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "export KEY=VALUE") != 0) return -1;
    char tmp[SHELL_MAX_ARG_LEN];
    strncpy(tmp, cmd->argv[1], sizeof(tmp) - 1);
    char *eq = strchr(tmp, '=');
    if (!eq) {
        shell_out_puts(out, "export: expected KEY=VALUE\n");
        return -1;
    }
    *eq++ = '\0';
    return shell_setenv(tmp, eq);
}

static int cmd_editor(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out)
{
    if (require_args(cmd, 2, out, "vim FILE") != 0) return -1;
    char path[PUREUNIX_MAX_PATH];
    abs_path(ctx, cmd->argv[1], path);
    return editor_open(path);
}
