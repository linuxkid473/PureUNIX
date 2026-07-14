#ifndef PUDE_FILES_H
#define PUDE_FILES_H

/* PUFiles: a real ring-3 graphical file manager for `pude` (docs/pude.md),
 * plugged in via the same app_class_t (user/pude_app.h) PUTerm and
 * Calculator use. Browses PureUNIX's real filesystem (VFS/EXT2/FAT16)
 * through ordinary newlib calls -- opendir/readdir/stat/mkdir/rmdir/
 * unlink/rename -- exactly like any real userspace program would; there is
 * no fake/mock directory tree anywhere in it. See user/pude_files.c for
 * the implementation and docs/pude.md's "PUFiles" section for the full
 * design writeup (layout, file-opening policy, widget reuse).
 */
#include "pude_app.h"

extern const app_class_t pufiles_app_class;

#endif
