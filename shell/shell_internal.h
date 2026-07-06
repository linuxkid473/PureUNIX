#ifndef PUREUNIX_SHELL_INTERNAL_H
#define PUREUNIX_SHELL_INTERNAL_H

#include <stdarg.h>
#include <pureunix/config.h>
#include <pureunix/types.h>

#define SHELL_MAX_ARGS 16
#define SHELL_MAX_ARG_LEN 96
#define SHELL_MAX_COMMANDS 4
#define SHELL_OUTPUT_CAP 16384

typedef struct shell_command {
    int argc;
    char args[SHELL_MAX_ARGS][SHELL_MAX_ARG_LEN];
    char *argv[SHELL_MAX_ARGS + 1];
    char input[PUREUNIX_MAX_PATH];
    char output[PUREUNIX_MAX_PATH];
    bool append;
} shell_command_t;

typedef struct shell_pipeline {
    int count;
    shell_command_t commands[SHELL_MAX_COMMANDS];
} shell_pipeline_t;

typedef struct shell_output {
    char *buffer;
    size_t capacity;
    size_t length;
    bool terminal;
} shell_output_t;

typedef struct shell_context {
    char cwd[PUREUNIX_MAX_PATH];
} shell_context_t;

typedef int (*builtin_fn_t)(shell_context_t *ctx, shell_command_t *cmd, const char *input, shell_output_t *out);

typedef struct builtin {
    const char *name;
    const char *summary;
    builtin_fn_t fn;
} builtin_t;

int shell_parse(const char *line, shell_pipeline_t *pipeline, shell_output_t *err);
int shell_readline(const char *prompt, char *out, size_t max);
void shell_history_add(const char *line);
size_t shell_history_count(void);
const char *shell_history_get(size_t index);

const builtin_t *shell_builtins(size_t *count);
const builtin_t *shell_find_builtin(const char *name);
void shell_out_write(shell_output_t *out, const char *data, size_t len);
void shell_out_puts(shell_output_t *out, const char *str);
void shell_out_printf(shell_output_t *out, const char *fmt, ...);

const char *shell_getenv(const char *key);
int shell_setenv(const char *key, const char *value);
void shell_env_list(shell_output_t *out);
char *const *shell_build_envp(void);

#endif
