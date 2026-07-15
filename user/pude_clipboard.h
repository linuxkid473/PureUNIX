#ifndef PUDE_CLIPBOARD_H
#define PUDE_CLIPBOARD_H

/* A tiny desktop-session-local, text-only clipboard shared by every `pude`
 * app -- the general mechanism PUText's Ctrl+C/X/V need, built the same
 * way pude_spawn.h's cross-app mailbox was: real shared mutable state in
 * its own .c file (not header-only like pude_gfx.h/pude_widgets.h, since
 * this genuinely is shared state between translation units, not a
 * stateless helper).
 *
 * "Desktop-session-local" means the data lives in `pude`'s own process
 * memory for as long as it's running -- there is no cross-reboot
 * persistence and no host-OS clipboard integration (this environment has
 * neither), exactly like every other cross-window mechanism in this
 * desktop (pude_spawn.h's mailbox, the window pool itself). Any app in the
 * same pude process can read what any other app last put here; nothing
 * about the API is PUText-specific, so a future app (PUTerm selection,
 * PUFiles multi-select copy/paste, ...) can use it the same way.
 */
#include <stdbool.h>
#include <stddef.h>

/* Replaces the clipboard's contents with a copy of `text` (exactly `len`
 * bytes, NOT required to be NUL-terminated -- callers with embedded NULs
 * are unlikely for a text editor, but the length is authoritative either
 * way). Frees whatever was previously stored. A `len` of 0 clears the
 * clipboard to "empty" (pude_clipboard_has_data() then returns false)
 * without freeing-then-immediately-failing on a malloc(0) edge case. */
void pude_clipboard_set(const char *text, size_t len);

/* Returns a pointer to the clipboard's internal buffer and writes its
 * length to *len_out -- valid only until the next pude_clipboard_set()
 * call; the caller must copy out anything it needs to keep. Returns NULL
 * (and sets *len_out = 0) if the clipboard is empty. */
const char *pude_clipboard_get(size_t *len_out);

bool pude_clipboard_has_data(void);

#endif
