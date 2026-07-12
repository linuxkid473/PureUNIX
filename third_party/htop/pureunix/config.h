#ifndef HTOP_PUREUNIX_CONFIG_H
#define HTOP_PUREUNIX_CONFIG_H
/*
config.h --hand-written for the PureUNIX cross build, in place of the one
htop's own `configure` would normally generate from config.h.in.

Why hand-written rather than running htop's own configure/autoreconf:
unlike ncurses (see docs/ncurses-port.md), htop's own build doesn't
generate any real source at build time --`my_htop_platform` selection
(configure.ac) just decides which small platform subdirectory's .c files
join one fixed, hand-enumerable core file list (Makefile.am's
`myhtopsources`), exactly the same shape as third_party/lua/'s object
list. Reproducing that selection directly in PureUNIX's own top-level
Makefile (see its "htop" section) needs no autoconf at all --the actual
autoconf *tests* this file replaces (AC_CHECK_FUNCS/AC_CHECK_HEADERS)
would all be cross-compiled anyway and mostly unrunnable, the same
"provide a known-good answer instead of an unrunnable probe" reasoning
tools/build-ncurses.sh already documents.

Every HAVE_ and HTOP_ macro left undefined here is a deliberate "no" --htop's
own generic code has portable fallbacks for nearly all of them (see e.g.
XUtils.c's own strnlen()/strchrnul() implementations, used automatically
when HAVE_STRNLEN/HAVE_STRCHRNUL aren't defined). Nothing here fakes a
capability PureUNIX doesn't have.
*/

/* Real, present headers --third_party/newlib ships every one of these. */
#define HAVE_DIRENT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* The real, non-wide ncurses this project ports (docs/ncurses-port.md) —
 * third_party/ncurses/i686-elf/include/ installs plain curses.h/term.h,
 * not an ncursesw/ subdirectory (--disable-widec). */
#define HAVE_CURSES_H 1
#define HAVE_TERM_H 1
#define HAVE_LIBNCURSES 1

/* PROCDIR is an absolute path, asserted as such by
 * ProcessTable_goThroughEntries() in every real (non-BSD) backend —
 * fs/procfs.c's own mount point. */
#define PROCDIR "/proc"

/* Settings.c's real per-user config file: $HOME/.config/htop/htoprc --
 * configure.ac's own --with-config default (real XDG convention, not
 * PureUNIX-specific). BusyBox ash sets HOME=/root for the root user
 * (kernel/main.c), and /root already exists on the persistent image
 * (see docs/lua-port.md's "missing /root/home/guest" fix), so this is a
 * real, writable, persisted path, not a fabricated one. */
#define CONFIGDIR "/.config"
/* System-wide fallback (Settings_read(SYSCONFDIR "/htoprc", ...)) --
 * matches BusyBox's own /etc/passwd convention (user/newlib_syscalls.c's
 * getpwuid()). No /etc/htoprc ships on PureUNIX, so this path is simply
 * never found and Settings_defaultMeters() takes over -- real, honest
 * fallback behavior, not a fake file. */
#define SYSCONFDIR "/etc"

#define PACKAGE "htop"
#define PACKAGE_NAME "htop"
#define PACKAGE_STRING "htop 3.5.1"
#define PACKAGE_TARNAME "htop"
#define PACKAGE_VERSION "3.5.1"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL ""
#define VERSION "3.5.1"
#define COPYRIGHT "(C) 2004-2025 Hisham Muhammad, htop dev team"

#endif
