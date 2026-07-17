#ifndef PUDE_QTCLIENT_H
#define PUDE_QTCLIENT_H

/* user/pude_qtclient.c — pude's app_class_t adapter for an external Qt GUI
 * client process (docs/qt-port.md Phase 6). See that file's own header
 * comment for the full design. */
#include "pude_app.h"

extern const app_class_t qtclient_app_class;
/* Same generic adapter as qtclient_app_class above (identical callback
 * set — see pude_qtclient_set_exec_path() below) registered a second
 * time under its own name/default size so pude's launcher menu can offer
 * two distinct real Qt binaries as two distinct entries, e.g. the
 * QRasterWindow smoke test (qtclient_app_class -> qtwindowtest.elf) and
 * a real QtWidgets app (this one -> qtwidgetstest.elf) — see
 * user/pude.c's g_apps[]. */
extern const app_class_t qtclient_widgets_app_class;

/* One-shot mailbox, same pattern as user/pude_term.h's
 * puterm_set_startup_command(): set right before requesting a spawn of
 * qtclient_app_class, consumed by the very next qtclient_create() call.
 * Names the real ELF to fork/exec (e.g. "/bin/qtwindowtest.elf") -- the
 * app_class_t interface itself has no per-instance "which program"
 * concept (every other app_class_t is a fixed, single program), so this
 * is the smallest way to let one generic adapter launch different Qt
 * client binaries without widening that interface for every existing
 * app. */
void pude_qtclient_set_exec_path(const char *path);

#endif
