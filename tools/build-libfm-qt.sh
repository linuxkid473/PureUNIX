#!/usr/bin/env bash
# Cross-builds third_party/libfm-qt/i686-elf (real libfm-qt6.a + headers)
# from the vendored upstream source under
# third_party/libfm-qt/libfm-qt-2.4.0/, applying the two real, minimal
# patches under third_party/libfm-qt/patches/:
#
#   0001-make-xcb-optional.patch — no X server/XCB exists on PureUnix at
#   all; makes find_package(XCB) optional and #ifdef-guards the one
#   narrow X11-only feature (XDS drag-and-drop) it gates, which upstream's
#   own code already runtime-no-ops outside a real "xcb" QPA platform.
#
#   0002-static-lib-qt653-skip-translations-tests.patch — three real,
#   independent fixes: (a) lowers QT_MINIMUM_VERSION to match our real
#   vendored Qt 6.5.3 (verified no 6.6-only API is actually used, by
#   building against these real headers), (b) skips Qt6LinguistTools/
#   translations entirely (that's the separate qttools submodule, never
#   cross-built for PureUnix — a real, disclosed English-only-UI
#   limitation), (c) builds the library STATIC instead of SHARED and
#   drops upstream's own tests/test-*.cpp interactive manual-test GUIs
#   (PureUnix has no dynamic loader at all; the test GUIs aren't a real
#   test suite and aren't needed for the real deliverable).
#
# Real dependency graph (this project's own top-level CMakeLists.txt,
# checked directly): Qt6Widgets + Qt6GuiPrivate, GLib/GIO/GObject/GThread,
# MenuCache, lxqt-menu-data, libexif, lxqt-build-tools (for its
# find-modules + LXQt CMake macros) — every one of these already vendored
# under third_party/*/i686-elf/ by their own build-*.sh scripts.
set -euo pipefail

LIBFMQT_VERSION=2.4.0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LIBFMQT_SRC="${REPO_ROOT}/third_party/libfm-qt/libfm-qt-${LIBFMQT_VERSION}"
PATCHES_DIR="${REPO_ROOT}/third_party/libfm-qt/patches"
VENDOR_DIR="${REPO_ROOT}/third_party/libfm-qt/i686-elf"
TOOLCHAIN_FILE="${REPO_ROOT}/tools/pureunix-libfmqt-toolchain.cmake"

QT_DIR="${REPO_ROOT}/third_party/qt/i686-elf"
GLIB_DIR="${REPO_ROOT}/third_party/glib/i686-elf"
MENUCACHE_DIR="${REPO_ROOT}/third_party/menu-cache/i686-elf"
LIBEXIF_DIR="${REPO_ROOT}/third_party/libexif/i686-elf"
LXQT_BUILD_TOOLS_DIR="${REPO_ROOT}/third_party/lxqt-build-tools/i686-elf"
LXQT_MENU_DATA_DIR="${REPO_ROOT}/third_party/lxqt-menu-data/i686-elf"
PCRE2_DIR="${REPO_ROOT}/third_party/pcre2/i686-elf"
LIBFFI_DIR="${REPO_ROOT}/third_party/libffi/i686-elf"

# glib-2.0.pc itself Requires: libpcre2-8, and gobject-2.0.pc Requires:
# libffi — pkg-config walks the full dependency chain even for a plain
# `pkg_check_modules(PC_MENUCACHE libmenu-cache)` (libmenu-cache.pc
# Requires: glib-2.0 transitively), so both must be on PKG_CONFIG_LIBDIR
# too or pkg-config fails the whole lookup and FindMenuCache.cmake's
# non-fallback MENUCACHE_INCLUDE_DIR (see its own file — unlike
# MENUCACHE_INCLUDE_DIRS it has no find_path() fallback at all) comes back
# empty, a hard configure error.
PKG_CONFIG_LIBDIR_VENDORED="${GLIB_DIR}/lib/pkgconfig:${MENUCACHE_DIR}/lib/pkgconfig:${PCRE2_DIR}/lib/pkgconfig:${LIBFFI_DIR}/lib/pkgconfig"

command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }
[ -d "${LIBFMQT_SRC}" ] || { echo "missing ${LIBFMQT_SRC} — vendor the libfm-qt-${LIBFMQT_VERSION} source first" >&2; exit 1; }
[ -d "${QT_DIR}/lib" ] || { echo "missing ${QT_DIR} — run tools/build-qt.sh first" >&2; exit 1; }
[ -d "${GLIB_DIR}/lib" ] || { echo "missing ${GLIB_DIR} — run tools/build-glib.sh first" >&2; exit 1; }
[ -d "${MENUCACHE_DIR}/lib" ] || { echo "missing ${MENUCACHE_DIR} — run tools/build-menu-cache.sh first" >&2; exit 1; }
[ -d "${LIBEXIF_DIR}/lib" ] || { echo "missing ${LIBEXIF_DIR} — run tools/build-libexif.sh first" >&2; exit 1; }
[ -d "${LXQT_BUILD_TOOLS_DIR}" ] || { echo "missing ${LXQT_BUILD_TOOLS_DIR} — run tools/build-lxqt-build-tools.sh first" >&2; exit 1; }
[ -d "${LXQT_MENU_DATA_DIR}" ] || { echo "missing ${LXQT_MENU_DATA_DIR} — run tools/build-lxqt-menu-data.sh first" >&2; exit 1; }

# Same real link-probe helper objects tools/build-qt.sh itself builds (see
# tools/pureunix-qt-toolchain.cmake's own LINKSTUB_DIR comment) — not
# actually expected to be exercised here (CMAKE_TRY_COMPILE_TARGET_TYPE is
# STATIC_LIBRARY, and patch 0002 already drops every real executable
# target from libfm-qt's own build), but built anyway as cheap insurance
# against any CMake-internal check that does try a real link.
echo "==> Building link-probe helper objects"
mkdir -p "${REPO_ROOT}/build/qt-linkstub"
# Matches the Makefile's own NEWLIB_CFLAGS exactly (the _POSIX_THREADS/
# _UNIX98_THREAD_MUTEX_ATTRIBUTES/_POSIX_READER_WRITER_LOCKS defines gate
# newlib's own pthread.h exposing pthread_mutexattr_t.type/
# pthread_rwlock_t/pthread_rwlockattr_t at all — without them
# newlib_syscalls.c's own pthread_rwlock_*/mutexattr_settype functions
# fail to compile).
LINKSTUB_CFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${REPO_ROOT}/third_party/newlib/i686-elf/include -D_POSIX_THREADS=1 -D_UNIX98_THREAD_MUTEX_ATTRIBUTES=1 -D_POSIX_READER_WRITER_LOCKS=1"
i686-elf-gcc -std=gnu99 -ffreestanding -O2 -m32 -march=i686 -fno-stack-protector -fno-pic -fno-pie \
  ${LINKSTUB_CFLAGS} \
  -c "${REPO_ROOT}/user/newlib_syscalls.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_syscalls.o"
i686-elf-gcc -std=gnu99 -ffreestanding -O2 -m32 -march=i686 -fno-stack-protector -fno-pic -fno-pie \
  ${LINKSTUB_CFLAGS} \
  -c "${REPO_ROOT}/user/newlib_crt0.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0.o"
i686-elf-gcc -m32 -c "${REPO_ROOT}/user/newlib_crt0.S" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0_asm.o"

WORK_DIR="$(mktemp -d /tmp/pureunix-libfmqt.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

# Checked out under a name other than "src" deliberately: libfm-qt's own
# top-level CMakeLists.txt does add_subdirectory(src), and with
# CMAKE_INCLUDE_CURRENT_DIR ON (which it also sets) that subdirectory's
# CMAKE_CURRENT_SOURCE_DIR gets added to every target's include dirs — if
# our own checkout dir were also named "src", the resulting real path
# would end in the literal (harmless-looking but CMake-rejected) "src/src/"
# and trip CMake's "in-source-tree path in an exported target's
# INTERFACE_INCLUDE_DIRECTORIES" generate-time error.
echo "==> Copying vendored source (pristine ${LIBFMQT_SRC} is never modified in place)"
cp -R "${LIBFMQT_SRC}" "${WORK_DIR}/libfm-qt-src"

for p in "${PATCHES_DIR}"/*.patch; do
	echo "==> Applying $(basename "${p}")"
	patch -p1 -d "${WORK_DIR}/libfm-qt-src" < "${p}"
done

BUILD_DIR="${WORK_DIR}/build"
INSTALL_ROOT="${WORK_DIR}/install"
mkdir -p "${BUILD_DIR}"

# PKG_CONFIG_LIBDIR (not just _PATH) fully sandboxes pkg-config away from
# this host's own real, system-installed glib2/libexif (real risk on a
# dev machine with Homebrew) — FindGLIB.cmake/FindMenuCache.cmake/
# FindExif.cmake (from lxqt-build-tools) all call pkg_check_modules()
# first and use its output only as a HINT for their own real find_path()/
# find_library() calls across CMAKE_FIND_ROOT_PATH, so even an empty
# PKG_CONFIG_LIBDIR (pkg-config module "not found") still resolves
# correctly via those vendored directories already being in
# CMAKE_FIND_ROOT_PATH (see tools/pureunix-libfmqt-toolchain.cmake) — but
# pointing PKG_CONFIG_LIBDIR at our own real vendored .pc files is
# strictly better (gives FindGLIB.cmake/FindMenuCache.cmake real HINTS).
# libexif has no vendored .pc file at all (tools/build-libexif.sh never
# generated one) — FindExif.cmake resolves it purely via the
# CMAKE_FIND_ROOT_PATH fallback (LIBEXIF_DIR/include/libexif/exif-data.h
# and LIBEXIF_DIR/lib/libexif.a match its NAMES/PATH_SUFFIXES directly).
echo "==> Configuring libfm-qt ${LIBFMQT_VERSION} (cross, via ${TOOLCHAIN_FILE})"
PKG_CONFIG_LIBDIR="${PKG_CONFIG_LIBDIR_VENDORED}" \
PKG_CONFIG_PATH="" \
cmake -S "${WORK_DIR}/libfm-qt-src" -B "${BUILD_DIR}" \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_DOCUMENTATION=OFF

echo "==> Building"
PKG_CONFIG_LIBDIR="${GLIB_DIR}/lib/pkgconfig:${MENUCACHE_DIR}/lib/pkgconfig" \
PKG_CONFIG_PATH="" \
cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "==> Installing (DESTDIR-redirected, see tools/build-lxqt-build-tools.sh's own comment on why)"
DESTDIR="${INSTALL_ROOT}" cmake --build "${BUILD_DIR}" --target install

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/." "${VENDOR_DIR}/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
