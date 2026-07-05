#ifndef VI_COMPAT_FCNTL_H
#define VI_COMPAT_FCNTL_H

/* Values match include/pureunix/fcntl.h / user/libpure.h exactly — open()
 * below passes flags straight through to pu_open(), no translation. */
#define O_RDONLY 0
#define O_WRONLY 0x001
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* mode (3rd arg) is accepted for source compatibility with upstream Neatvi
 * (open(path, O_WRONLY|O_CREAT, conf_mode())) but ignored: PureUNIX's
 * pu_open() has no mode parameter — new files get the filesystem's default
 * permissions. */
int open(const char *path, int flags, ...);

#endif
