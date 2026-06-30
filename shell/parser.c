#include "shell_internal.h"
#include <pureunix/string.h>

static void shell_error(shell_output_t *err, const char *msg)
{
    if (err) {
        shell_out_puts(err, msg);
        shell_out_puts(err, "\n");
    }
}

static void command_init(shell_command_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
}

static bool is_meta(char c)
{
    return c == '|' || c == '<' || c == '>';
}

static const char *next_token(const char **cursor, char *token, size_t token_size)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (!*p) {
        *cursor = p;
        return NULL;
    }

    size_t len = 0;
    if (is_meta(*p)) {
        token[len++] = *p++;
        if (token[0] == '>' && *p == '>') {
            token[len++] = *p++;
        }
    } else if (*p == '\'' || *p == '"') {
        char quote = *p++;
        while (*p && *p != quote && len + 1 < token_size) {
            token[len++] = *p++;
        }
        if (*p == quote) {
            ++p;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t' && !is_meta(*p) && len + 1 < token_size) {
            token[len++] = *p++;
        }
    }
    token[len] = '\0';
    *cursor = p;
    return token;
}

int shell_parse(const char *line, shell_pipeline_t *pipeline, shell_output_t *err)
{
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->count = 1;
    command_init(&pipeline->commands[0]);
    shell_command_t *cmd = &pipeline->commands[0];
    const char *cursor = line;
    char tok[SHELL_MAX_ARG_LEN];

    while (next_token(&cursor, tok, sizeof(tok))) {
        if (strcmp(tok, "|") == 0) {
            if (cmd->argc == 0) {
                shell_error(err, "syntax error near '|'");
                return -1;
            }
            if (pipeline->count >= SHELL_MAX_COMMANDS) {
                shell_error(err, "pipeline too long");
                return -1;
            }
            cmd = &pipeline->commands[pipeline->count++];
            command_init(cmd);
            continue;
        }

        if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            bool input = tok[0] == '<';
            bool append = strcmp(tok, ">>") == 0;
            if (!next_token(&cursor, tok, sizeof(tok))) {
                shell_error(err, "redirection missing filename");
                return -1;
            }
            if (input) {
                strncpy(cmd->input, tok, sizeof(cmd->input) - 1);
            } else {
                strncpy(cmd->output, tok, sizeof(cmd->output) - 1);
                cmd->append = append;
            }
            continue;
        }

        if (cmd->argc >= SHELL_MAX_ARGS) {
            shell_error(err, "too many arguments");
            return -1;
        }
        strncpy(cmd->args[cmd->argc], tok, SHELL_MAX_ARG_LEN - 1);
        cmd->argv[cmd->argc] = cmd->args[cmd->argc];
        cmd->argc++;
        cmd->argv[cmd->argc] = NULL;
    }

    if (pipeline->commands[pipeline->count - 1].argc == 0) {
        if (pipeline->count == 1) {
            return 0;
        }
        shell_error(err, "empty command at end of pipeline");
        return -1;
    }
    return 0;
}
