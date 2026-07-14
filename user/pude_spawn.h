#ifndef PUDE_SPAWN_H
#define PUDE_SPAWN_H

/* A tiny one-slot mailbox letting any pude app ask the WM (user/pude.c) to
 * open a brand-new window for a different app class -- e.g. PUFiles asking
 * for a fresh PUTerm window preloaded with an editor command when the user
 * opens a plain-text file (docs/pude.md's "Opening files" section).
 *
 * Only user/pude.c's spawn_window() may actually allocate a window-pool
 * slot/z-order entry, so an app can't just call it directly; this is the
 * smallest general mechanism that lets an app *request* a spawn without
 * user/pude.c having to know anything app-specific about who's asking or
 * why. Real storage lives in pude_spawn.c (not header-only like
 * pude_gfx.h/pude_widgets.h) because it's genuine shared mutable state
 * between two different translation units, not a stateless helper.
 */
#include "pude_app.h"
#include <stddef.h>

#define PUDE_SPAWN_CMD_MAX 256

/* Requests that the WM spawn a new window of class `cls` on its next
 * frame. `startup_command`, if non-NULL and non-empty, is passed through
 * for apps that support one (currently only PUTerm --
 * puterm_set_startup_command()); ignored by apps that don't. Overwrites
 * any not-yet-drained previous request (there is only ever one WM main
 * loop draining this once per frame, so in practice nothing is lost). */
void pude_request_spawn(const app_class_t *cls, const char *startup_command);

/* Called once per WM frame by user/pude.c. Returns true and fills
 * *cls_out/cmd_out (cmd_out always NUL-terminated, "" if none was given)
 * if a request was pending, clearing it either way. */
bool pude_take_spawn_request(const app_class_t **cls_out, char *cmd_out, size_t cap);

#endif
