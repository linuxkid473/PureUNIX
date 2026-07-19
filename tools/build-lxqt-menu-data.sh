#!/usr/bin/env bash
# "Builds" third_party/lxqt-menu-data/i686-elf (real .directory/.menu
# freedesktop.org data files) from the vendored upstream source under
# third_party/lxqt-menu-data/lxqt-menu-data-2.4.0/.
#
# Real dependency for libfm-qt (docs/pcmanfm-port.md phase 7) — data
# files only (menu-category .directory entries + real .menu XML files),
# zero compiled code, same reasoning as tools/build-lxqt-build-tools.sh
# (whose own real lxqt2-build-tools-config.cmake + lxqt_translate_desktop()
# macro this package's own CMakeLists.txt uses to generate its real
# output). Configured/installed with the host's own native cmake (no
# cross-toolchain needed — nothing here is ever linked into a target
# binary).
set -euo pipefail

LXQT_MENU_DATA_VERSION=2.4.0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC="${REPO_ROOT}/third_party/lxqt-menu-data/lxqt-menu-data-${LXQT_MENU_DATA_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/lxqt-menu-data/i686-elf"
LXQT_BUILD_TOOLS_DIR="${REPO_ROOT}/third_party/lxqt-build-tools/i686-elf"

command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }
[ -d "${SRC}" ] || { echo "missing ${SRC} — vendor the lxqt-menu-data-${LXQT_MENU_DATA_VERSION} source first" >&2; exit 1; }
[ -d "${LXQT_BUILD_TOOLS_DIR}" ] || { echo "missing ${LXQT_BUILD_TOOLS_DIR} — run tools/build-lxqt-build-tools.sh first" >&2; exit 1; }

WORK_DIR="$(mktemp -d /tmp/pureunix-lxqtmenudata.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine ${SRC} is never modified in place)"
cp -R "${SRC}" "${WORK_DIR}/src"

BUILD_DIR="${WORK_DIR}/build"
INSTALL_ROOT="${WORK_DIR}/install"
mkdir -p "${BUILD_DIR}"

# CMAKE_INSTALL_PREFIX=/usr (a plain, real, standard prefix — not our own
# scratch dir) + DESTDIR at install time (the real, standard mechanism
# every absolute install(DESTINATION) path still respects, same
# technique tools/build-glib.sh already relies on for Meson's own
# equivalent prefix=/usr mismatch) — using our own scratch dir as
# *both* CMAKE_INSTALL_PREFIX *and* DESTDIR would double-nest every
# prefix-relative path (DESTDIR prepends to CMAKE_INSTALL_PREFIX too,
# not just to genuinely-absolute destinations).
echo "==> Configuring lxqt-menu-data ${LXQT_MENU_DATA_VERSION} (host-native, data files only)"
cmake -S "${WORK_DIR}/src" -B "${BUILD_DIR}" \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="${LXQT_BUILD_TOOLS_DIR}"

echo "==> Installing (DESTDIR-redirected, see this script's own comment)"
DESTDIR="${INSTALL_ROOT}" cmake --build "${BUILD_DIR}" --target install

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/." "${VENDOR_DIR}/"
# The real .menu files themselves land under the genuinely-absolute
# /etc/xdg/menus (LXQT_ETC_XDG_DIR, not prefix-relative — see this
# script's own comment above), so DESTDIR places them at
# ${INSTALL_ROOT}/etc/menus rather than under .../usr/ with everything
# else; copied separately, into the same real /etc/xdg layout PureUnix
# itself will use at runtime.
mkdir -p "${VENDOR_DIR}/etc"
cp -R "${INSTALL_ROOT}/etc/." "${VENDOR_DIR}/etc/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
