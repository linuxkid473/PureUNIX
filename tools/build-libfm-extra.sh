#!/usr/bin/env bash
# Builds third_party/libfm-extra/i686-elf (headers + libfm-extra.a) from
# the vendored upstream source under
# third_party/libfm-extra/libfm-1.3.2/, configured with --with-extra-only.
#
# Bootstrap prerequisite for MenuCache (docs/pcmanfm-port.md phase 6) —
# libmenu-cache's own menu-cache-gen generator tool depends on
# libfm-extra, a small subset of the (much larger, GTK-based) libfm
# project containing only the non-GUI utility code (XDG desktop-entry
# parsing, path/URI helpers) — real upstream-documented circular-
# dependency workaround (libfm-1.3.2/README's own --with-extra-only
# note), not invented here.
#
# 1.3.2 (not the newer 1.4.1 git tag) deliberately: 1.4.1 has no
# published release tarball (only a raw git snapshot needing gtk-doc/
# intltool host tools this environment doesn't have to bootstrap
# autotools from configure.ac) — 1.3.2 is the latest version with a
# real, properly released tarball that already ships a pre-generated
# `configure` script, avoiding that whole extra host-tooling dependency
# for a small, stable, years-old library where the extra/GIO-facing API
# hasn't materially changed.
#
# Same vendoring pattern as every other out-of-tree dependency in this
# repo: run once, offline, cross-compiling with the same i686-elf-gcc
# and newlib every other PureUNIX userspace binary uses, and commit the
# resulting headers + static lib so `make`/`make iso` stay network-free.
set -euo pipefail

LIBFM_VERSION=1.3.2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LIBFM_SRC="${REPO_ROOT}/third_party/libfm-extra/libfm-${LIBFM_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/libfm-extra/i686-elf"
GLIB_DIR="${REPO_ROOT}/third_party/glib/i686-elf"
PCRE2_DIR="${REPO_ROOT}/third_party/pcre2/i686-elf"
LIBFFI_DIR="${REPO_ROOT}/third_party/libffi/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
[ -d "${LIBFM_SRC}" ] || { echo "missing ${LIBFM_SRC} — vendor the libfm-${LIBFM_VERSION} source first" >&2; exit 1; }
[ -d "${GLIB_DIR}/lib" ] || { echo "missing ${GLIB_DIR} — run tools/build-glib.sh first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-libfm-extra.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/libfm-extra/libfm-${LIBFM_VERSION}/ is never modified in place)"
cp -R "${LIBFM_SRC}" "${WORK_DIR}/src"

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

TARGET_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
TARGET_LDFLAGS="-T ${REPO_ROOT}/user/linker.ld -nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

# GIO_CFLAGS/GIO_LIBS set directly (bypassing pkg-config's own
# PKG_CHECK_MODULES lookup, which autoconf skips entirely once these
# ${MODULE}_CFLAGS/${MODULE}_LIBS env vars are already set): our
# vendored GLib's own .pc files (third_party/glib/i686-elf/lib/
# pkgconfig/*.pc) bake in prefix=/usr (see tools/build-glib.sh's own
# --prefix=/usr, needed for THAT build's own vendoring step) — a real
# path mismatch on this host, not something PKG_CONFIG_LIBDIR alone can
# fix, since /usr/include/glib-2.0 doesn't exist here. Real values,
# just supplied directly instead of round-tripping through pkg-config.
GIO_CFLAGS="-I${GLIB_DIR}/include/glib-2.0 -I${GLIB_DIR}/lib/glib-2.0/include"
GIO_LIBS="-L${GLIB_DIR}/lib -L${PCRE2_DIR}/lib -L${LIBFFI_DIR}/lib -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lgthread-2.0 -lintl -lz -lpcre2-8 -lffi"
# Real: GLib 2.88.2 (vendored) is >> 2.27.0, so the scheme-handler
# feature genuinely should be enabled — same override reasoning as GIO
# above, not a fake "assume yes".
GLIB2_27_CFLAGS="${GIO_CFLAGS}"
GLIB2_27_LIBS="${GIO_LIBS}"

BUILD_TRIPLE="$("${WORK_DIR}/src/config.guess" 2>/dev/null || echo unknown)"
if [ "${BUILD_TRIPLE}" = "unknown" ]; then
	BUILD_TRIPLE="$(/usr/bin/env config.guess 2>/dev/null || uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
fi

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring libfm-extra ${LIBFM_VERSION} for i686-elf (cross, --with-extra-only)"
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
GIO_CFLAGS="${GIO_CFLAGS}" \
GIO_LIBS="${GIO_LIBS}" \
GLIB2_27_CFLAGS="${GLIB2_27_CFLAGS}" \
GLIB2_27_LIBS="${GLIB2_27_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--disable-shared \
	--disable-nls \
	--with-extra-only \
	--prefix=/usr

echo "==> Building libfm-extra only (real per-file compiles; may hit"
echo "    libtool's own final-archive-link limitation, same as"
echo "    tools/build-libffi.sh — checked for specifically below, not"
echo "    blanket-ignored)"
BUILD_LOG="${WORK_DIR}/build.log"
if make -C src -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" libfm-extra.la 2>&1 | tee "${BUILD_LOG}"; then
	MAKE_OK=1
else
	MAKE_OK=0
fi

if [ "${MAKE_OK}" -eq 0 ]; then
	if ! grep -q "cannot build libtool library" "${BUILD_LOG}"; then
		echo "make failed for a reason other than the known libtool-archival limitation — real build failure, see ${BUILD_LOG} above" >&2
		exit 1
	fi
	REAL_ERRORS=$(grep -E '(^|[^a-zA-Z])error:' "${BUILD_LOG}" | grep -vc "cannot build libtool library" || true)
	if [ "${REAL_ERRORS}" -gt 0 ]; then
		echo "make failed with ${REAL_ERRORS} real compile error(s) in addition to the known libtool-archival limitation — see ${BUILD_LOG} above" >&2
		exit 1
	fi
	echo "==> make's only failure was the known libtool-archival limitation; every real .o compiled fine"
fi

LIB_OBJS=$(find src/extra -maxdepth 1 -name '*.o')
if [ -z "${LIB_OBJS}" ]; then
	echo "no libfm-extra object files were produced — real build failure, see output above" >&2
	exit 1
fi

echo "==> Archiving libfm-extra.a directly (bypassing libtool's own final link if it failed)"
if [ ! -f src/.libs/libfm-extra.a ]; then
	"${CROSS}ar" rcs libfm-extra.a ${LIB_OBJS}
	"${CROSS}ranlib" libfm-extra.a
	LIBFILE="libfm-extra.a"
else
	LIBFILE="src/.libs/libfm-extra.a"
fi

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}/include/libfm" "${VENDOR_DIR}/lib"
# Real public headers for --with-extra-only mode, exactly matching
# src/Makefile.am's own LIBFM_EXTRA_INCLUDES list (checked directly, not
# guessed) — not the whole src/ tree, which also has GTK-only headers
# never even compiled in this mode.
cp "${WORK_DIR}/src/src/fm-extra.h" "${VENDOR_DIR}/include/libfm/"
cp "${WORK_DIR}/src/src/fm-version.h" "${VENDOR_DIR}/include/libfm/"
cp "${WORK_DIR}/src/src/extra/fm-xml-file.h" "${VENDOR_DIR}/include/libfm/"
cp "${LIBFILE}" "${VENDOR_DIR}/lib/libfm-extra.a"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
