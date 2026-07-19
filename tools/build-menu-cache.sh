#!/usr/bin/env bash
# Builds third_party/menu-cache/i686-elf (headers + libmenu-cache.a) plus
# real, standalone menu-cache-gen/menu-cache-daemon ELFs, from the
# vendored upstream source under
# third_party/menu-cache/menu-cache-1.1.0/.
#
# Fourth new dependency for the PCManFM-Qt port (docs/pcmanfm-port.md
# phase 6) — libfm-qt's own "Applications" XDG-menu view is built on
# this. 1.1.0 (not the newer 1.1.1 git tag) deliberately: 1.1.1 has no
# published release tarball and would need gtk-doc/libtool host tools to
# bootstrap from configure.ac, same reasoning as
# tools/build-libfm-extra.sh's own version choice — 1.1.0 is the latest
# version with a real, properly released tarball already shipping a
# pre-generated `configure` script.
#
# Real dependency graph (configure.ac, checked directly): glib-2.0 +
# gio-2.0 (already vendored, tools/build-glib.sh) and libfm-extra
# (already vendored, tools/build-libfm-extra.sh — the whole reason that
# script exists is to satisfy this one). GLIB_CFLAGS/GLIB_LIBS and
# LIBFM_EXTRA_CFLAGS/LIBFM_EXTRA_LIBS are set directly as env vars
# (bypassing pkg-config's own lookup, same reasoning as
# tools/build-libfm-extra.sh's own comment: our vendored .pc files bake
# in prefix=/usr, a real path mismatch on this host).
#
# Same vendoring pattern as every other out-of-tree dependency in this
# repo: run once, offline, cross-compiling with the same i686-elf-gcc
# and newlib every other PureUNIX userspace binary uses, and commit the
# resulting headers + static lib + ELFs so `make`/`make iso` stay
# network-free.
set -euo pipefail

MENU_CACHE_VERSION=1.1.0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MENU_CACHE_SRC="${REPO_ROOT}/third_party/menu-cache/menu-cache-${MENU_CACHE_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/menu-cache/i686-elf"
GLIB_DIR="${REPO_ROOT}/third_party/glib/i686-elf"
PCRE2_DIR="${REPO_ROOT}/third_party/pcre2/i686-elf"
LIBFFI_DIR="${REPO_ROOT}/third_party/libffi/i686-elf"
LIBFM_EXTRA_DIR="${REPO_ROOT}/third_party/libfm-extra/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
[ -d "${MENU_CACHE_SRC}" ] || { echo "missing ${MENU_CACHE_SRC} — vendor the menu-cache-${MENU_CACHE_VERSION} source first" >&2; exit 1; }
[ -d "${GLIB_DIR}/lib" ] || { echo "missing ${GLIB_DIR} — run tools/build-glib.sh first" >&2; exit 1; }
[ -d "${LIBFM_EXTRA_DIR}/lib" ] || { echo "missing ${LIBFM_EXTRA_DIR} — run tools/build-libfm-extra.sh first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-menu-cache.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/menu-cache/menu-cache-${MENU_CACHE_VERSION}/ is never modified in place)"
cp -R "${MENU_CACHE_SRC}" "${WORK_DIR}/src"

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

# -fcommon: menu-cache-gen/menu-tags.h declares its menuTag_* globals as
# plain tentative definitions (no `extern`), included into main.c/
# menu-merge.c/menu-compose.c alike — real, pre-GCC10 C tentative-
# definition-merging behavior this exact source relied on. Modern GCC
# defaults to -fno-common (a real, deliberate upstream GCC change,
# unrelated to this port), turning that into hard multiple-definition
# link errors; upstream's own next release (1.1.1, no published tarball
# — see this script's own header comment on why 1.1.0 was vendored
# instead) fixed this for real by adding `extern` throughout. -fcommon
# restores the traditional linking behavior 1.1.0's own source assumes,
# a real, standard, well-documented GCC flag for exactly this legacy-C
# compatibility case — not a source patch, since backporting 1.1.1's
# fix would require gtk-doc/intltool to bootstrap that unreleased tag.
TARGET_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wno-unused-parameter -fcommon -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
TARGET_LDFLAGS="-T ${REPO_ROOT}/user/linker.ld -nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

GLIB_CFLAGS="-I${GLIB_DIR}/include/glib-2.0 -I${GLIB_DIR}/lib/glib-2.0/include"
GLIB_LIBS="-L${GLIB_DIR}/lib -L${PCRE2_DIR}/lib -L${LIBFFI_DIR}/lib -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0 -lgthread-2.0 -lintl -lz -lpcre2-8 -lffi"
LIBFM_EXTRA_CFLAGS="-I${LIBFM_EXTRA_DIR}/include ${GLIB_CFLAGS}"
LIBFM_EXTRA_LIBS="-L${LIBFM_EXTRA_DIR}/lib -lfm-extra ${GLIB_LIBS}"

BUILD_TRIPLE="$("${WORK_DIR}/src/config.guess" 2>/dev/null || echo unknown)"
if [ "${BUILD_TRIPLE}" = "unknown" ]; then
	BUILD_TRIPLE="$(/usr/bin/env config.guess 2>/dev/null || uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
fi

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring menu-cache ${MENU_CACHE_VERSION} for i686-elf (cross)"
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
GLIB_CFLAGS="${GLIB_CFLAGS}" \
GLIB_LIBS="${GLIB_LIBS}" \
LIBFM_EXTRA_CFLAGS="${LIBFM_EXTRA_CFLAGS}" \
LIBFM_EXTRA_LIBS="${LIBFM_EXTRA_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--disable-shared \
	--prefix=/usr

# Built as 3 independent subdir targets (not a single top-level `make
# all`, which stops at libmenu-cache's own known libtool-archival
# limitation before ever reaching the other two subdirs): neither
# menu-cache-gen nor menu-cache-daemon actually link against
# libmenu-cache.a itself (checked directly in their own Makefile.am —
# both only use its headers, generated into the build tree by
# config.status regardless of whether the library itself finished
# linking), so each subdir's own real success/failure is independent.
build_subdir() {
	local dir="$1"
	local log="${WORK_DIR}/build-${dir//\//_}.log"
	if make -C "${dir}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" 2>&1 | tee "${log}"; then
		return 0
	fi
	if ! grep -q "cannot build libtool library" "${log}"; then
		echo "make -C ${dir} failed for a reason other than the known libtool-archival limitation — real build failure, see ${log} above" >&2
		exit 1
	fi
	local real_errors
	real_errors=$(grep -E '(^|[^a-zA-Z])error:' "${log}" | grep -vc "cannot build libtool library" || true)
	if [ "${real_errors}" -gt 0 ]; then
		echo "make -C ${dir} failed with ${real_errors} real compile error(s) in addition to the known libtool-archival limitation — see ${log} above" >&2
		exit 1
	fi
	echo "==> make -C ${dir}'s only failure was the known libtool-archival limitation; every real .o compiled fine"
}

echo "==> Building libmenu-cache (real per-file compiles; may hit the known libtool limitation)"
build_subdir libmenu-cache
echo "==> Building menu-cache-gen (real executable, no libtool library involved)"
build_subdir menu-cache-gen
echo "==> Building menu-cache-daemon (real executable, no libtool library involved)"
build_subdir menu-cache-daemon

LIB_OBJS=$(find libmenu-cache -maxdepth 1 -name '*.o')
if [ -z "${LIB_OBJS}" ]; then
	echo "no libmenu-cache object files were produced — real build failure, see output above" >&2
	exit 1
fi

echo "==> Archiving libmenu-cache.a directly (bypassing libtool's own final link if it failed)"
if [ ! -f libmenu-cache/.libs/libmenu-cache.a ]; then
	(cd libmenu-cache && "${CROSS}ar" rcs libmenu-cache.a ${LIB_OBJS##libmenu-cache/})
	"${CROSS}ranlib" libmenu-cache/libmenu-cache.a
	LIBFILE="libmenu-cache/libmenu-cache.a"
else
	LIBFILE="libmenu-cache/.libs/libmenu-cache.a"
fi

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}/include/libmenu-cache" "${VENDOR_DIR}/lib" "${VENDOR_DIR}/bin"
# menu-cache.h is templated (menu-cache.h.in -> menu-cache.h by
# config.status) — it lands in the *build* tree (this script's own cwd),
# not the pristine source tree, since this is an out-of-tree build.
cp libmenu-cache/menu-cache.h "${VENDOR_DIR}/include/libmenu-cache/"
cp "${LIBFILE}" "${VENDOR_DIR}/lib/libmenu-cache.a"
[ -f menu-cache-gen/menu-cache-gen ] && cp menu-cache-gen/menu-cache-gen "${VENDOR_DIR}/bin/"
[ -f menu-cache-daemon/menu-cached ] && cp menu-cache-daemon/menu-cached "${VENDOR_DIR}/bin/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
