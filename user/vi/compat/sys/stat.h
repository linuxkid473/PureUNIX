#ifndef VI_COMPAT_SYS_STAT_H
#define VI_COMPAT_SYS_STAT_H

/* ex.c only reads st_mtime (to detect a file changing on disk under it);
 * everything else about PureUNIX's real stat() is irrelevant here. */
struct stat {
    long st_mtime;
};

int stat(const char *path, struct stat *st);

#endif
