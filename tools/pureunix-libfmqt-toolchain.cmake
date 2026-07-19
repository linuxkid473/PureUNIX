# CMake cross-compilation toolchain file for cross-building libfm-qt
# (docs/pcmanfm-port.md phase 7) against PureUnix's own cross toolchain.
# Reuses tools/pureunix-qt-toolchain.cmake verbatim (the same real
# i686-elf-gcc/g++ + newlib + libstdc++ setup that already successfully
# cross-built Qt6 itself) and layers on top of it the additional real
# vendored dependencies libfm-qt itself needs: Qt6's own installed CMake
# package config (Qt6WidgetsConfig.cmake, a real product of that same Qt
# build), GLib/GObject/GIO, MenuCache, libexif, and lxqt-build-tools'
# own CMake find-modules/macros (a real, pure-CMake package with no
# compiled code at all — see tools/build-lxqt-build-tools.sh's own
# comment).
#
# CMAKE_FIND_ROOT_PATH_MODE_LIBRARY/_INCLUDE are already ONLY (set by
# the included Qt toolchain file) — every one of these vendored
# directories must be added to CMAKE_FIND_ROOT_PATH itself (not just
# CMAKE_PREFIX_PATH) for find_library()/find_path() (which is what
# lxqt-build-tools' own Find*.cmake modules actually use, not a direct
# pkg-config prefix lookup — real, checked directly in those files, not
# assumed) to actually locate the real files here, regardless of
# whatever prefix our own vendored .pc files happen to bake in (a real,
# already-hit mismatch for pkg-config-based lookups elsewhere in this
# port — irrelevant here since these modules only use pkg-config output
# as a search *hint*, then do their own real filesystem find_library/
# find_path across CMAKE_FIND_ROOT_PATH-derived paths).
include("${CMAKE_CURRENT_LIST_DIR}/pureunix-qt-toolchain.cmake")

set(QT_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/qt/i686-elf")
set(GLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/glib/i686-elf")
set(PCRE2_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/pcre2/i686-elf")
set(LIBFFI_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libffi/i686-elf")
set(MENUCACHE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/menu-cache/i686-elf")
set(LIBEXIF_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libexif/i686-elf")
set(LXQT_BUILD_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/lxqt-build-tools/i686-elf")
set(LXQT_MENU_DATA_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/lxqt-menu-data/i686-elf")

list(APPEND CMAKE_FIND_ROOT_PATH
    "${QT_DIR}"
    "${GLIB_DIR}"
    "${PCRE2_DIR}"
    "${LIBFFI_DIR}"
    "${MENUCACHE_DIR}"
    "${LIBEXIF_DIR}"
    "${LXQT_BUILD_TOOLS_DIR}"
    "${LXQT_MENU_DATA_DIR}"
)
list(APPEND CMAKE_PREFIX_PATH
    "${QT_DIR}"
    "${GLIB_DIR}"
    "${MENUCACHE_DIR}"
    "${LIBEXIF_DIR}"
    "${LXQT_BUILD_TOOLS_DIR}"
    "${LXQT_MENU_DATA_DIR}"
)
# lxqt-build-tools' own find-modules/ (FindGLIB.cmake/FindMenuCache.cmake/
# FindExif.cmake — the real modules libfm-qt's own find_package() calls
# resolve to, since none of these three ship their own CMake config
# files the way Qt6/lxqt-build-tools itself does) plus cmake/modules/
# (LXQtCompilerSettings.cmake etc, pulled in via `include(...)` once
# find_package(lxqt2-build-tools) sets CMAKE_MODULE_PATH itself — listed
# here too since that find_package() call needs to already be
# resolvable to something before it can do that).
list(APPEND CMAKE_MODULE_PATH
    "${LXQT_BUILD_TOOLS_DIR}/share/cmake/lxqt2-build-tools/find-modules"
    "${LXQT_BUILD_TOOLS_DIR}/share/cmake/lxqt2-build-tools/modules"
)
