#!/usr/bin/env bash
# Builds third_party/libexif/i686-elf (headers + libexif.a) from the
# vendored upstream source under third_party/libexif/libexif-0.6.26/.
#
# Third new dependency for the PCManFM-Qt port (docs/pcmanfm-port.md
# phase 6) — libfm-qt uses it for JPEG EXIF orientation when generating
# thumbnails. By far the easiest of the new dependencies researched for
# this port: no GLib, no pthread, no sockets/IPC, no XDG environment
# dependency at all — a pure metadata-parsing library that only ever
# calls fopen()/fread() and does its own byte-level parsing (confirmed
# by reading the real upstream source, not assumed).
#
# Same vendoring pattern as every other out-of-tree dependency in this
# repo (tools/build-pcre2.sh/build-libffi.sh): run once, offline,
# cross-compiling with the same i686-elf-gcc and newlib every other
# PureUNIX userspace binary uses, and commit the resulting headers +
# static lib so `make`/`make iso` stay network-free.
set -euo pipefail

LIBEXIF_VERSION=0.6.26

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LIBEXIF_SRC="${REPO_ROOT}/third_party/libexif/libexif-${LIBEXIF_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/libexif/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
[ -d "${LIBEXIF_SRC}" ] || { echo "missing ${LIBEXIF_SRC} — vendor the libexif-${LIBEXIF_VERSION} source first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-libexif.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/libexif/libexif-${LIBEXIF_VERSION}/ is never modified in place)"
cp -R "${LIBEXIF_SRC}" "${WORK_DIR}/src"

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

# Matches USER_CFLAGS + NEWLIB_CFLAGS in the top-level Makefile exactly, so
# the resulting libexif.a is flag-for-flag consistent with every other
# newlib-linked PureUNIX binary it gets linked into.
TARGET_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
# -T user/linker.ld: see tools/build-libffi.sh's own comment/
# gotcha_libffi_cross_build_linker_ld_libtool.md — newlib_crt0.c
# unconditionally references TLS section-boundary symbols only defined
# there, needed for configure's own bootstrap "does the compiler work"
# link check to succeed at all.
TARGET_LDFLAGS="-T ${REPO_ROOT}/user/linker.ld -nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

BUILD_TRIPLE="$("${WORK_DIR}/src/config.guess" 2>/dev/null || echo unknown)"
if [ "${BUILD_TRIPLE}" = "unknown" ]; then
	BUILD_TRIPLE="$(/usr/bin/env config.guess 2>/dev/null || uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
fi

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring libexif ${LIBEXIF_VERSION} for i686-elf (cross)"
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--disable-shared \
	--disable-nls \
	--disable-docs \
	--disable-dependency-tracking \
	--prefix=/usr

echo "==> Building the core library only (real per-file compiles; may hit"
echo "    libtool's own final-archive-link limitation, same as"
echo "    tools/build-libffi.sh — checked for specifically below, not"
echo "    blanket-ignored)"
BUILD_LOG="${WORK_DIR}/build.log"
if make -C libexif -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" libexif.la 2>&1 | tee "${BUILD_LOG}"; then
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

LIB_OBJS=$(find libexif -name '*.o')
if [ -z "${LIB_OBJS}" ]; then
	echo "no libexif object files were produced — real build failure, see output above" >&2
	exit 1
fi

echo "==> Archiving libexif.a directly (bypassing libtool's own final link if it failed)"
if [ ! -f libexif/.libs/libexif.a ]; then
	"${CROSS}ar" rcs libexif.a ${LIB_OBJS}
	"${CROSS}ranlib" libexif.a
	LIBFILE="libexif.a"
else
	LIBFILE="libexif/.libs/libexif.a"
fi

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}/include/libexif" "${VENDOR_DIR}/lib"
find "${WORK_DIR}/src/libexif" -name '*.h' | while read -r hdr; do
	rel="${hdr#${WORK_DIR}/src/libexif/}"
	mkdir -p "${VENDOR_DIR}/include/libexif/$(dirname "${rel}")"
	cp "${hdr}" "${VENDOR_DIR}/include/libexif/${rel}"
done
cp "${LIBFILE}" "${VENDOR_DIR}/lib/libexif.a"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
