#ifndef PUDE_TEXT_H
#define PUDE_TEXT_H

/* PUText: a real ring-3 graphical text editor for `pude` (docs/pude.md),
 * plugged in via the same app_class_t (user/pude_app.h) PUTerm/Calculator/
 * PUFiles use. See user/pude_text.c for the implementation (document
 * buffer, cursor/selection model, rendering, keyboard/mouse input, the
 * embedded file picker for Open/Save As) and docs/pude.md's "PUText"
 * section for the full design writeup.
 */
#include "pude_app.h"

extern const app_class_t putext_app_class;

/* Queues a path to open in the *next* PUText window the instant it's
 * created -- e.g. PUFiles double-clicking a text file (docs/pude.md's
 * "Opening files" section), exactly the same "type-ahead into a fresh
 * instance" pattern user/pude_term.h's puterm_set_startup_command() uses
 * for PUTerm, routed through the same pude_spawn.h mailbox. Consumed and
 * cleared by the very next putext_create() call; leave unset (or pass
 * NULL/"") for a plain new Untitled document. */
void putext_set_startup_path(const char *path);

#endif
