/* user/pude_clipboard.c -- see pude_clipboard.h. */
#include "pude_clipboard.h"
#include <stdlib.h>
#include <string.h>

static struct {
    char *data;
    size_t len;
} g_clip;

void pude_clipboard_set(const char *text, size_t len)
{
    free(g_clip.data);
    g_clip.data = NULL;
    g_clip.len = 0;
    if (len == 0 || !text) {
        return;
    }
    g_clip.data = malloc(len);
    if (!g_clip.data) {
        return; /* clipboard just stays empty -- caller's copy/cut is a no-op */
    }
    memcpy(g_clip.data, text, len);
    g_clip.len = len;
}

const char *pude_clipboard_get(size_t *len_out)
{
    if (len_out) {
        *len_out = g_clip.len;
    }
    return g_clip.len > 0 ? g_clip.data : NULL;
}

bool pude_clipboard_has_data(void)
{
    return g_clip.len > 0;
}
