#!/usr/bin/env bash
# Builds third_party/libffi/i686-elf (headers + libffi.a) from the vendored
# upstream source under third_party/libffi/libffi-3.7.1/.
#
# libffi is the first new dependency for the PCManFM-Qt port
# (docs/pcmanfm-port.md) — GObject's generic signal-marshalling machinery
# links against it directly. Same vendoring pattern as every other
# out-of-tree build in this repo (tools/build-ncurses.sh/build-libstdcxx.sh):
# run once, offline, cross-compiling with the same i686-elf-gcc and newlib
# every other PureUNIX userspace binary uses, and commit the resulting
# headers + static lib so `make`/`make iso` stay network-free.
#
# libffi's own configure.host has a generic `i?86-*-* | x86_64-*-* | amd64-*`
# fallback case that matches i686-elf directly (no PureUNIX-specific target
# triple needed there) and picks TARGET=X86 (32-bit) — real, upstream,
# unmodified support, not something added for this port.
#
# One real platform question worth documenting rather than assuming: libffi's
# closure support (src/closures.c, used for GObject's C-callback trampolines)
# defaults to mmap()-based writable+executable memory only on __linux__ or
# Windows; every other target (including this one — PureUNIX defines neither)
# falls through to its "on many systems, memory returned by malloc is
# writable and executable, so just use it" fallback path. That's a genuine,
# accurate description of this kernel's own page tables (every PT_LOAD
# segment, and every sbrk()-grown heap page riding on the same mapping, is
# mapped writable with no W^X/NX enforcement anywhere — see
# gotcha_fixed_heap_va_collision.md's own note on this), not a hack layered
# on top — this Just Works with zero source changes.
set -euo pipefail

LIBFFI_VERSION=3.7.1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LIBFFI_SRC="${REPO_ROOT}/third_party/libffi/libffi-${LIBFFI_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/libffi/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
[ -d "${LIBFFI_SRC}" ] || { echo "missing ${LIBFFI_SRC} — vendor the libffi-${LIBFFI_VERSION} source first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-libffi.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/libffi/libffi-${LIBFFI_VERSION}/ is never modified in place)"
cp -R "${LIBFFI_SRC}" "${WORK_DIR}/src"

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

# Matches USER_CFLAGS + NEWLIB_CFLAGS in the top-level Makefile exactly, so
# the resulting libffi.a is flag-for-flag consistent with every other
# newlib-linked PureUNIX binary it gets linked into.
TARGET_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
# -T user/linker.ld (not just -nostdlib): newlib_crt0.c unconditionally
# references TLS section-boundary symbols (__tbss_end/__tdata_end/...,
# added to user/linker.ld during the Qt6 port's real i386 TLS support,
# docs/qt-port.md) whether or not the linked program itself uses
# thread_local at all -- configure's basic "does the compiler work" conftest
# link fails with "undefined reference to __tbss_end" etc. without this,
# a real, general gotcha for any future vendored-library build script that
# predates TLS and doesn't know to pass this.
TARGET_LDFLAGS="-T ${REPO_ROOT}/user/linker.ld -nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

BUILD_TRIPLE="$("${WORK_DIR}/src/config.guess")"

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring libffi ${LIBFFI_VERSION} for i686-elf (cross)"
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--disable-shared \
	--disable-multi-os-directory \
	--disable-docs \
	--prefix=/usr

echo "==> Compiling (all real object files — expected to fail at the final"
echo "    libtool .la link step, see this script's own note below)"
# libtool refuses its own final "libffi.la"/"libffi_convenience.la" link
# step here ("cannot build libtool library ... from non-libtool objects on
# this host"): it objects to TARGET_LIBS' plain crt0/newlib_syscalls .o
# files being present on a libtool link line at all, even for a
# --disable-shared static-only build. Those objects are only needed so
# configure's own "does this compiler make a real executable" bootstrap
# probe succeeds (this platform is always -nostdlib, see this script's own
# TARGET_LDFLAGS comment) -- they were never meant to end up *inside*
# libffi.a itself, and autoconf provides no way to scope LIBS/LDFLAGS to
# just that one bootstrap check. Every real per-file compile (prep_cif.o,
# types.o, raw_api.o, java_raw_api.o, closures.o, tramp.o, x86/ffi.o,
# x86/sysv.o -- libfm_la_SOURCES in the real upstream Makefile.am, plus
# configure.host's default x86 TARGETDIR sources) succeeds fully before
# this; only the final archival step fails. Real fix: skip libtool's own
# archival and do it directly with the real cross ar/ranlib, exactly what
# libtool would have done internally anyway for a static-only library.
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" || true

LIBFFI_OBJS="src/prep_cif.o src/types.o src/raw_api.o src/java_raw_api.o src/closures.o src/tramp.o src/x86/ffi.o src/x86/sysv.o"
for obj in ${LIBFFI_OBJS}; do
	[ -f "${obj}" ] || { echo "expected object ${obj} missing — libffi's own source list changed, update LIBFFI_OBJS above" >&2; exit 1; }
done

echo "==> Archiving libffi.a directly (bypassing libtool's own broken final link)"
"${CROSS}ar" rcs libffi.a ${LIBFFI_OBJS}
"${CROSS}ranlib" libffi.a

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}/include" "${VENDOR_DIR}/lib"
cp "${WORK_DIR}/build/include/ffi.h" "${WORK_DIR}/build/include/ffitarget.h" "${VENDOR_DIR}/include/"
cp libffi.a "${VENDOR_DIR}/lib/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
