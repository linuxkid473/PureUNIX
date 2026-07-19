#!/usr/bin/env bash
# Thin wrapper around i686-elf-gcc that strips -pthread before exec'ing
# the real compiler — used only for cross-compiling GLib (tools/build-
# glib.sh, docs/pcmanfm-port.md phase 3/6).
#
# Meson's built-in `dependency('threads')` unconditionally appends
# -pthread to both compile and link flags on any non-Darwin/non-Windows
# target, with no cross-file property to opt out (a known, common
# real-world meson-cross-compilation friction point, not specific to
# this port). Our i686-elf-gcc was never configured to understand that
# flag at all ("unrecognized command-line option '-pthread'") — it's a
# glibc-toolchain convenience flag (defines _REENTRANT, links -lpthread)
# neither of which this target needs: _POSIX_THREADS is already set
# globally (see the Makefile's own NEWLIB_CFLAGS comment) and the real
# pthread implementation (user/newlib_syscalls.c) is already unavoidably
# linked into every program via newlib_crt0/newlib_syscalls.o, not a
# separate -lpthread archive. Stripping the one flag our compiler
# rejects is the smallest fix; every other argument passes through
# completely unmodified.
set -euo pipefail
args=()
for arg in "$@"; do
	if [ "${arg}" != "-pthread" ]; then
		args+=("${arg}")
	fi
done
exec i686-elf-gcc "${args[@]}"
