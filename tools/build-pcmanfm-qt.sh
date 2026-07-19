#!/usr/bin/env bash
# Cross-builds third_party/pcmanfm-qt/i686-elf/bin/pcmanfm-qt (the real
# upstream file manager executable) from the vendored upstream source
# under third_party/pcmanfm-qt/pcmanfm-qt-2.4.0/, applying the one real,
# minimal patch under third_party/pcmanfm-qt/patches/:
#
#   0001-make-dbus-layershellqt-xcb-optional.patch — makes three real,
#   hard `REQUIRED` dependencies optional, none of which are buildable on
#   PureUnix at all:
#     - Qt6DBus: a separate qtbase module never cross-built here (only
#       Core+Gui+Widgets were — see third_party/qt/README.md); there is
#       no real D-Bus daemon/library on this platform. Real, disclosed
#       cost: no single-instance enforcement, no org.freedesktop.FileManager1
#       service — every launch opens its own new instance, matching how
#       PUDE itself launches one fresh process per app-open anyway.
#     - LayerShellQt: Wayland layer-shell protocol library, never vendored
#       (PureUnix has neither X11 nor Wayland); its one real use
#       (desktop-background layer/anchors setup) was already
#       runtime-guarded behind Application::underWayland(), which is
#       unconditionally false here.
#     - XCB: not even find_package()-d by upstream (real desktop Linux
#       systems already have libxcb on the system linker path); its one
#       real use (an EWMH window-type hint) was already runtime-guarded
#       behind `QGuiApplication::platformName() == "xcb"`.
#   Also drops two genuinely unused/dead X11 includes (<X11/Xlib.h> in
#   application.cpp, <xcb/xcb.h> in desktopwindow.h) and skips
#   translations (Qt6LinguistTools/qttools was never cross-built either,
#   same as libfm-qt's own patch).
#
# Real dependency graph (this project's own top-level CMakeLists.txt,
# checked directly): Qt6Widgets, fm-qt6 (libfm-qt6.a, just cross-built),
# lxqt2-build-tools (for its find-modules + LXQt CMake macros).
set -euo pipefail

PCMANFMQT_VERSION=2.4.0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PCMANFMQT_SRC="${REPO_ROOT}/third_party/pcmanfm-qt/pcmanfm-qt-${PCMANFMQT_VERSION}"
PATCHES_DIR="${REPO_ROOT}/third_party/pcmanfm-qt/patches"
VENDOR_DIR="${REPO_ROOT}/third_party/pcmanfm-qt/i686-elf"
TOOLCHAIN_FILE="${REPO_ROOT}/tools/pureunix-pcmanfm-toolchain.cmake"

QT_DIR="${REPO_ROOT}/third_party/qt/i686-elf"
GLIB_DIR="${REPO_ROOT}/third_party/glib/i686-elf"
MENUCACHE_DIR="${REPO_ROOT}/third_party/menu-cache/i686-elf"
LIBEXIF_DIR="${REPO_ROOT}/third_party/libexif/i686-elf"
LXQT_BUILD_TOOLS_DIR="${REPO_ROOT}/third_party/lxqt-build-tools/i686-elf"
LXQT_MENU_DATA_DIR="${REPO_ROOT}/third_party/lxqt-menu-data/i686-elf"
PCRE2_DIR="${REPO_ROOT}/third_party/pcre2/i686-elf"
LIBFFI_DIR="${REPO_ROOT}/third_party/libffi/i686-elf"
LIBFMQT_DIR="${REPO_ROOT}/third_party/libfm-qt/i686-elf"

# Same real sandboxing reasoning as tools/build-libfm-qt.sh's own comment.
PKG_CONFIG_LIBDIR_VENDORED="${GLIB_DIR}/lib/pkgconfig:${MENUCACHE_DIR}/lib/pkgconfig:${PCRE2_DIR}/lib/pkgconfig:${LIBFFI_DIR}/lib/pkgconfig"

command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }
[ -d "${PCMANFMQT_SRC}" ] || { echo "missing ${PCMANFMQT_SRC} — vendor the pcmanfm-qt-${PCMANFMQT_VERSION} source first" >&2; exit 1; }
[ -d "${QT_DIR}/lib" ] || { echo "missing ${QT_DIR} — run tools/build-qt.sh first" >&2; exit 1; }
[ -d "${LIBFMQT_DIR}/lib" ] || { echo "missing ${LIBFMQT_DIR} — run tools/build-libfm-qt.sh first" >&2; exit 1; }
[ -d "${LXQT_BUILD_TOOLS_DIR}" ] || { echo "missing ${LXQT_BUILD_TOOLS_DIR} — run tools/build-lxqt-build-tools.sh first" >&2; exit 1; }

echo "==> Building link-probe helper objects"
mkdir -p "${REPO_ROOT}/build/qt-linkstub"
LINKSTUB_CFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${REPO_ROOT}/third_party/newlib/i686-elf/include -D_POSIX_THREADS=1 -D_UNIX98_THREAD_MUTEX_ATTRIBUTES=1 -D_POSIX_READER_WRITER_LOCKS=1"
i686-elf-gcc -std=gnu99 -ffreestanding -O2 -m32 -march=i686 -fno-stack-protector -fno-pic -fno-pie \
  ${LINKSTUB_CFLAGS} \
  -c "${REPO_ROOT}/user/newlib_syscalls.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_syscalls.o"
i686-elf-gcc -std=gnu99 -ffreestanding -O2 -m32 -march=i686 -fno-stack-protector -fno-pic -fno-pie \
  ${LINKSTUB_CFLAGS} \
  -c "${REPO_ROOT}/user/newlib_crt0.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0.o"
i686-elf-gcc -m32 -c "${REPO_ROOT}/user/newlib_crt0.S" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0_asm.o"

WORK_DIR="$(mktemp -d /tmp/pureunix-pcmanfmqt.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

# Checked out under a name other than "src"/"pcmanfm" (this project's own
# top-level dir is literally named "pcmanfm", and CMAKE_INCLUDE_CURRENT_DIR
# is ON here too) -- same real "in-source-tree path in an exported
# target's INTERFACE_INCLUDE_DIRECTORIES" risk tools/build-libfm-qt.sh's
# own comment already hit once; pcmanfm-qt builds an executable (not an
# exported library target) so it likely doesn't apply, but there's no
# reason to risk the same coincidence twice.
echo "==> Copying vendored source (pristine ${PCMANFMQT_SRC} is never modified in place)"
cp -R "${PCMANFMQT_SRC}" "${WORK_DIR}/pcmanfm-qt-src"

for p in "${PATCHES_DIR}"/*.patch; do
	echo "==> Applying $(basename "${p}")"
	patch -p1 -d "${WORK_DIR}/pcmanfm-qt-src" < "${p}"
done

BUILD_DIR="${WORK_DIR}/build"
INSTALL_ROOT="${WORK_DIR}/install"
mkdir -p "${BUILD_DIR}"

echo "==> Configuring pcmanfm-qt ${PCMANFMQT_VERSION} (cross, via ${TOOLCHAIN_FILE})"
PKG_CONFIG_LIBDIR="${PKG_CONFIG_LIBDIR_VENDORED}" \
PKG_CONFIG_PATH="" \
cmake -S "${WORK_DIR}/pcmanfm-qt-src" -B "${BUILD_DIR}" \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_DOCUMENTATION=OFF

echo "==> Building"
PKG_CONFIG_LIBDIR="${PKG_CONFIG_LIBDIR_VENDORED}" \
PKG_CONFIG_PATH="" \
cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "==> Installing (DESTDIR-redirected, see tools/build-lxqt-build-tools.sh's own comment on why)"
DESTDIR="${INSTALL_ROOT}" cmake --build "${BUILD_DIR}" --target install

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/." "${VENDOR_DIR}/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
