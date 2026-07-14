/* user/pude_spawn.c -- see pude_spawn.h. */
#include "pude_spawn.h"
#include <string.h>

static struct {
    const app_class_t *cls;
    bool pending;
    char command[PUDE_SPAWN_CMD_MAX];
} g_request;

void pude_request_spawn(const app_class_t *cls, const char *startup_command)
{
    g_request.cls = cls;
    if (startup_command && startup_command[0]) {
        strncpy(g_request.command, startup_command, PUDE_SPAWN_CMD_MAX - 1);
        g_request.command[PUDE_SPAWN_CMD_MAX - 1] = '\0';
    } else {
        g_request.command[0] = '\0';
    }
    g_request.pending = true;
}

bool pude_take_spawn_request(const app_class_t **cls_out, char *cmd_out, size_t cap)
{
    if (!g_request.pending) {
        return false;
    }
    g_request.pending = false;
    *cls_out = g_request.cls;
    if (cmd_out && cap > 0) {
        strncpy(cmd_out, g_request.command, cap - 1);
        cmd_out[cap - 1] = '\0';
    }
    return true;
}
