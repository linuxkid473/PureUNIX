#ifndef PUDE_SETTINGS_H
#define PUDE_SETTINGS_H

/* Settings: a small ring-3 GUI app for `pude` (docs/pude.md) exposing
 * exactly one setting today -- the desktop wallpaper (user/pude_
 * wallpaper.c/.h) -- plugged into the WM through the same app_class_t
 * (user/pude_app.h) every other app uses. See user/pude_settings.c for
 * the implementation. */
#include "pude_app.h"

extern const app_class_t settings_app_class;

#endif
