#!/usr/bin/env bash
# Builds third_party/ncurses/i686-elf (headers + libncurses.a) from the
# vendored upstream source under third_party/ncurses/ncurses-6.5/.
#
# Why this exists (unlike TCC/Lua/SQLite's "compile a flat file list with our
# own Makefile rules, no upstream build system" pattern): ncurses' own build
# generates a substantial amount of its own source at build time — curses.h/
# term.h from templates substituted with configure-derived values, capability
# tables from its Caps database via awk scripts, and (via --with-fallbacks)
# a compiled-in terminfo table produced by running its own `tic` over a
# terminfo source file. Hand-reproducing that in a PureUNIX-authored Makefile
# section would mean re-deriving hundreds of generated lines by hand instead
# of vendoring them — not a faithful reproduction the way Lua's object list
# is. So, exactly like third_party/newlib (tools/build-newlib.sh) — the one
# other vendored dependency in this tree with the same "real build system,
# real generated sources" shape — ncurses' own configure/make is run once,
# here, cross-compiling with the same i686-elf-gcc and newlib CFLAGS every
# other PureUNIX userspace binary uses, and the result (headers + static
# lib) is vendored into third_party/ncurses/i686-elf/ and committed, so
# `make`/`make iso` themselves stay network-free and never invoke autotools.
#
# Requires: the i686-elf cross compiler already used to build PureUNIX
# itself, a native host C/C++ compiler (builds ncurses' own code-generator
# helper programs — make_hash, make_keys, and a host tic/infocmp for
# compiling the --with-fallbacks terminfo table — via --with-build-cc; these
# never run on the target, only during this build), and this script's own
# `make build/user/newlib_crt0*.o build/user/newlib_syscalls.o` prerequisite
# (built automatically below) so ncurses' configure-time link checks
#(`i686-elf-gcc conftest.c -o conftest`) resolve against a real PureUNIX
# _start/syscall layer instead of failing with "cannot create executables"
# — PureUNIX programs are always -nostdlib (they supply their own crt0), so
# a plain hosted-style link every other configure script assumes doesn't
# work here without it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NCURSES_SRC="${REPO_ROOT}/third_party/ncurses/ncurses-6.5"
TERMINFO_SRC="${REPO_ROOT}/third_party/ncurses/pureunix.terminfo"
VENDOR_DIR="${REPO_ROOT}/third_party/ncurses/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
command -v cc >/dev/null || { echo "a native host cc not found on PATH" >&2; exit 1; }
[ -d "${NCURSES_SRC}" ] || { echo "missing ${NCURSES_SRC} — vendor the ncurses-6.5 source first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-ncurses.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/ncurses/ncurses-6.5/ is never modified in place)"
cp -R "${NCURSES_SRC}" "${WORK_DIR}/src"

echo "==> Appending the pureunix terminfo entry to the working copy's misc/terminfo.src"
# A data addition (a new terminal description), not a source patch — see
# this script's own header comment and docs/ncurses-port.md. The pristine
# vendored copy under third_party/ncurses/ncurses-6.5/ never sees this.
cat "${TERMINFO_SRC}" >> "${WORK_DIR}/src/misc/terminfo.src"

# --with-fallbacks compiles the named terminfo entries straight into
# libncurses.a by running `tic`/`infocmp` over misc/terminfo.src at build
# time (MKfallback.sh) — without --with-tic-path/--with-infocmp-path below,
# configure falls back to whatever `tic`/`infocmp` are on $PATH, which on
# macOS is the ancient system /usr/bin/tic (a much older ncurses release)
# and chokes partway through this source file with "error writing ...".
# Building a *matching* host tic/infocmp from this exact vendored source
# first — same technique as this script's own cross build, just for the
# host instead of i686-elf — avoids that version mismatch entirely.
echo "==> Building a native host tic/infocmp from the same source (for --with-fallbacks table generation)"
mkdir -p "${WORK_DIR}/host-build"
(
	cd "${WORK_DIR}/host-build"
	"${WORK_DIR}/src/configure" \
		--without-shared --without-debug --without-ada \
		--without-cxx --without-cxx-binding \
		--without-manpages --without-tests \
		--disable-widec >/dev/null
	# Plain default `make` (not a direct `make progs/tic` sub-target): the
	# generated private headers (curses.priv.h, progs.priv.h, ...) that
	# progs/tic.c needs are only guaranteed built first when make walks
	# the whole tree in its own dependency order.
	make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null
)
HOST_TIC="${WORK_DIR}/host-build/progs/tic"
HOST_INFOCMP="${WORK_DIR}/host-build/progs/infocmp"
[ -x "${HOST_TIC}" ] && [ -x "${HOST_INFOCMP}" ] || { echo "host tic/infocmp build failed" >&2; exit 1; }

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

# Matches USER_CFLAGS + NEWLIB_CFLAGS in the top-level Makefile exactly, so
# the resulting libncurses.a is flag-for-flag consistent with every other
# newlib-linked PureUNIX binary it gets linked into.
TARGET_CFLAGS="-std=gnu99 -ffreestanding ${NCURSES_DEBUG_CFLAGS:--O2} -Wall -Wextra -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -Iinclude -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
# -nostdlib (every PureUNIX program supplies its own crt0) means configure's
# own "does this compiler make executables" conftest link needs our real
# crt0/syscalls objects supplied as LIBS (linked after conftest.o, resolving
# _start/libc symbols) rather than assuming a hosted default one exists.
TARGET_LDFLAGS="-nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

BUILD_TRIPLE="$("${NCURSES_SRC}/config.guess")"

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring ncurses 6.5 for i686-elf (cross, --with-build-cc)"
# cf_cv_working_poll: ncurses' configure (aclocal.m4's CF_FUNC_POLL) probes
# whether poll() *actually* works with an AC_TRY_RUN — a real pipe/socket,
# a real poll() call, checked at runtime. That can't execute while cross-
# compiling (the compiled probe is an i686-elf binary, unrunnable on the
# host), so autoconf falls back to its "unknown" cross-compiling answer —
# never "yes" — which permanently disables USE_FUNC_POLL (curses.priv.h)
# for the whole build, not just this one probe. The effect showed up as a
# real, reproducible bug: kgetch()'s escape-sequence trie matcher
# (ncurses/base/lib_getch.c) calls _nc_timed_wait() between bytes of a
# multi-byte function-key sequence to check "is the rest of this sequence
# available yet"; with USE_FUNC_POLL off, _nc_timed_wait() never even
# calls poll() and unconditionally reports "nothing more available",
# so every arrow-key/function-key press was returned as 2-3 separate raw
# bytes (ESC, '[', 'A') instead of one KEY_UP — confirmed by adding
# temporary trace prints to a scratch copy of lib_getch.c/lib_twait.c and
# watching kgetch()'s own loop give up after exactly one byte every time,
# then tracing that to USE_FUNC_POLL=0 in _nc_timed_wait's own trace line.
# PureUNIX's real poll() (user/newlib_syscalls.c) *is* correct for exactly
# this use — every fd it's asked about is always genuinely readable
# without blocking forever, since the underlying read() never blocks when
# data is truly pending — so seeding this cache variable is answering the
# question truthfully for this platform, not papering over a real gap.
cf_cv_working_poll=yes \
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--with-build-cc=cc \
	--without-shared \
	--without-debug \
	--without-profile \
	--without-cxx \
	--without-cxx-binding \
	--without-ada \
	--without-manpages \
	--without-tests \
	--without-progs \
	--disable-db-install \
	--disable-home-terminfo \
	--disable-database \
	--with-fallbacks=pureunix,dumb \
	--with-tic-path="${HOST_TIC}" \
	--with-infocmp-path="${HOST_INFOCMP}" \
	--disable-widec \
	--disable-rpath \
	--disable-pc-files \
	--without-pkg-config \
	--prefix=/usr

echo "==> Building (library only — progs/tests/cxx/ada all disabled above)"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" libs

echo "==> Installing into a scratch DESTDIR (curated public headers/libs only —"
echo "    ncurses' own 'install' target, not an ad-hoc header cherry-pick)"
INSTALL_ROOT="${WORK_DIR}/install"
make install DESTDIR="${INSTALL_ROOT}" >/dev/null

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/include" "${VENDOR_DIR}/include"
cp -R "${INSTALL_ROOT}/usr/lib" "${VENDOR_DIR}/lib"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
