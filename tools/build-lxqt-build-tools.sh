#!/usr/bin/env bash
# "Builds" third_party/lxqt-build-tools/i686-elf (real CMake find-modules/
# macros + a real, valid lxqt2-build-tools-config.cmake) from the
# vendored upstream source under
# third_party/lxqt-build-tools/lxqt-build-tools-2.4.0/.
#
# Bootstrap dependency for libfm-qt (docs/pcmanfm-port.md phase 7) — a
# real, pure-CMake package (find-modules/macros/templates, zero compiled
# code — confirmed by reading its own CMakeLists.txt/cmake/ tree, not
# assumed) that provides the FindGLIB.cmake/FindMenuCache.cmake/
# FindExif.cmake modules libfm-qt's own find_package() calls resolve to,
# plus real build-system macros (LXQtCompilerSettings,
# LXQtTranslateTs, ...) its CMakeLists.txt uses directly.
#
# Configured and installed with the HOST's own native cmake/compiler
# (no cross-toolchain file at all) — deliberately, not an oversight:
# nothing here is ever actually linked into a target binary, so there is
# no real cross-compilation to do; CMake's own project() call still
# wants *some* working C/CXX compiler to satisfy its default language
# checks even though nothing gets compiled, and the host's own is the
# simplest real one available.
set -euo pipefail

LXQTBT_VERSION=2.4.0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LXQTBT_SRC="${REPO_ROOT}/third_party/lxqt-build-tools/lxqt-build-tools-${LXQTBT_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/lxqt-build-tools/i686-elf"

command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }
[ -d "${LXQTBT_SRC}" ] || { echo "missing ${LXQTBT_SRC} — vendor the lxqt-build-tools-${LXQTBT_VERSION} source first" >&2; exit 1; }

WORK_DIR="$(mktemp -d /tmp/pureunix-lxqtbt.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine ${LXQTBT_SRC} is never modified in place)"
cp -R "${LXQTBT_SRC}" "${WORK_DIR}/src"

BUILD_DIR="${WORK_DIR}/build"
INSTALL_ROOT="${WORK_DIR}/install"
mkdir -p "${BUILD_DIR}"

# CMAKE_INSTALL_PREFIX=/usr (a plain, real, standard prefix), install-
# time DESTDIR redirecting the physical write into our own scratch dir
# (same technique tools/build-glib.sh already relies on) — deliberately
# NOT our own scratch INSTALL_ROOT as the *prefix* itself: several of
# these variables (LXQT_SHARE_DIR/LXQT_TRANSLATIONS_DIR/
# LXQT_GRAPHICS_DIR/LXQT_DATA_DIR) get baked as real, permanent
# preprocessor -D defines into LXQtConfigVars.cmake for any later real
# C++ consumer (libfm-qt, pcmanfm-qt itself) to read data files from at
# *runtime* on the real PureUnix target — baking in this build's own
# temporary /tmp path (deleted the moment this script exits) would leave
# every later consumer looking for real data files at a path that never
# existed on the target at all. LXQT_ETC_XDG_DIR is set explicitly for
# the same reason (its own default autodetection would otherwise query
# this *host's* unrelated Qt install via `qtpaths`, baking in a
# meaningless host path instead of PureUnix's own real "/etc/xdg").
echo "==> Configuring lxqt-build-tools ${LXQTBT_VERSION} (host-native, no compiled code)"
cmake -S "${WORK_DIR}/src" -B "${BUILD_DIR}" \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_BUILD_TYPE=Release \
	-DLXQT_ETC_XDG_DIR=/etc/xdg

echo "==> Installing (DESTDIR-redirected, see this script's own comment)"
DESTDIR="${INSTALL_ROOT}" cmake --build "${BUILD_DIR}" --target install

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/." "${VENDOR_DIR}/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
