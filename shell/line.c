#include "shell_internal.h"
#include <pureunix/keyboard.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

#define HISTORY_MAX 64
#define LINE_MAX 256

static char history[HISTORY_MAX][LINE_MAX];
static size_t history_len;

size_t shell_history_count(void)
{
    return history_len;
}

const char *shell_history_get(size_t index)
{
    return index < history_len ? history[index] : "";
}

void shell_history_add(const char *line)
{
    if (!line || !*line) {
        return;
    }
    if (history_len && strcmp(history[history_len - 1], line) == 0) {
        return;
    }
    if (history_len == HISTORY_MAX) {
        memmove(history[0], history[1], (HISTORY_MAX - 1) * LINE_MAX);
        history_len--;
    }
    strncpy(history[history_len++], line, LINE_MAX - 1);
}

static void redraw(const char *prompt, const char *line, size_t cursor)
{
    printf("\r\033[K%s%s", prompt, line);
    size_t len = strlen(line);
    for (size_t i = cursor; i < len; ++i) {
        putchar('\b');
    }
}

typedef struct completion_ctx {
    const char *prefix;
    char match[PUREUNIX_MAX_NAME];
    size_t count;
} completion_ctx_t;

static int complete_dir_cb(const vfs_dirent_t *entry, void *ctxp)
{
    completion_ctx_t *ctx = ctxp;
    size_t plen = strlen(ctx->prefix);
    if (strncmp(entry->name, ctx->prefix, plen) == 0) {
        if (ctx->count == 0) {
            strncpy(ctx->match, entry->name, sizeof(ctx->match) - 1);
        }
        ctx->count++;
    }
    return 0;
}

static void complete_line(char *line, size_t *len, size_t *cursor, const char *prompt)
{
    if (*cursor != *len) {
        return;
    }
    size_t start = *cursor;
    while (start > 0 && line[start - 1] != ' ') {
        start--;
    }
    const char *prefix = line + start;
    completion_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.prefix = prefix;

    size_t builtin_count;
    const builtin_t *builtins = shell_builtins(&builtin_count);
    for (size_t i = 0; i < builtin_count; ++i) {
        if (strncmp(builtins[i].name, prefix, strlen(prefix)) == 0) {
            if (ctx.count == 0) {
                strncpy(ctx.match, builtins[i].name, sizeof(ctx.match) - 1);
            }
            ctx.count++;
        }
    }

    if (ctx.count == 0) {
        vfs_readdir("/", complete_dir_cb, &ctx);
    }

    if (ctx.count == 1) {
        size_t prefix_len = strlen(prefix);
        size_t match_len = strlen(ctx.match);
        while (prefix_len < match_len && *len + 1 < LINE_MAX) {
            line[*len] = ctx.match[prefix_len++];
            (*len)++;
            (*cursor)++;
        }
        line[*len] = '\0';
    }
    redraw(prompt, line, *cursor);
}

int shell_readline(const char *prompt, char *out, size_t max)
{
    char line[LINE_MAX];
    size_t len = 0;
    size_t cursor = 0;
    size_t history_cursor = history_len;
    memset(line, 0, sizeof(line));
    printf("%s", prompt);

    for (;;) {
        int key = keyboard_getkey();
        if (key == KEY_ENTER) {
            putchar('\n');
            line[len] = '\0';
            strncpy(out, line, max - 1);
            out[max - 1] = '\0';
            shell_history_add(out);
            return (int)len;
        }
        if (key == KEY_CTRL_C) {
            printf("^C\n");
            out[0] = '\0';
            return 0;
        }
        if (key == KEY_BACKSPACE) {
            if (cursor > 0) {
                memmove(&line[cursor - 1], &line[cursor], len - cursor + 1);
                cursor--;
                len--;
                redraw(prompt, line, cursor);
            }
            continue;
        }
        if (key == KEY_DELETE) {
            if (cursor < len) {
                memmove(&line[cursor], &line[cursor + 1], len - cursor);
                len--;
                redraw(prompt, line, cursor);
            }
            continue;
        }
        if (key == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                putchar('\b');
            }
            continue;
        }
        if (key == KEY_RIGHT) {
            if (cursor < len) {
                putchar(line[cursor++]);
            }
            continue;
        }
        if (key == KEY_HOME) {
            cursor = 0;
            redraw(prompt, line, cursor);
            continue;
        }
        if (key == KEY_END) {
            cursor = len;
            redraw(prompt, line, cursor);
            continue;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            if (key == KEY_UP && history_cursor > 0) {
                history_cursor--;
            } else if (key == KEY_DOWN && history_cursor < history_len) {
                history_cursor++;
            }
            if (history_cursor < history_len) {
                strncpy(line, history[history_cursor], sizeof(line) - 1);
                len = strlen(line);
            } else {
                line[0] = '\0';
                len = 0;
            }
            cursor = len;
            redraw(prompt, line, cursor);
            continue;
        }
        if (key == KEY_TAB) {
            complete_line(line, &len, &cursor, prompt);
            continue;
        }
        if (key >= 32 && key < 127 && len + 1 < sizeof(line)) {
            memmove(&line[cursor + 1], &line[cursor], len - cursor + 1);
            line[cursor] = (char)key;
            cursor++;
            len++;
            redraw(prompt, line, cursor);
        }
    }
}
