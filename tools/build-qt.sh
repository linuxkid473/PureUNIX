#!/usr/bin/env bash
# Builds third_party/qt/i686-elf from real upstream Qt 6.5.3 (qtbase only:
# Core, Gui, Widgets - see docs/qt-port.md for why this version and these
# modules were picked, and the full list of what's enabled/disabled).
#
# Same "vendor a prebuilt build of real upstream source" pattern as
# tools/build-newlib.sh / tools/build-libstdcxx.sh: qtbase's full source
# (~280MB, far larger than any other third_party/ port) is fetched into a
# scratch directory and never committed - only the resulting headers +
# static libraries (~65MB) are vendored, run once, offline.
#
# Two real, minimal upstream Qt patches are applied (see
# third_party/qt/patches/, and docs/qt-port.md's patch list for why each is
# needed) - both are genuine bugs/gaps in qtbase itself, not scattered
# __PUREUNIX__ conditionals: adding a Q_OS_PUREUNIX case to Qt's own OS
# detection (mirroring how Q_OS_VXWORKS already works - set from the
# mkspec, not autodetected, since PureUnix has no predefined compiler
# macro), and a missing #include <grp.h> in qfilesystemengine_unix.cpp
# (relies on <pwd.h> transitively pulling it in, which doesn't happen with
# newlib's stricter header separation - a real bug on any such libc).
#
# Requires: the same i686-elf-g++ used to build PureUnix itself, the
# vendored newlib (tools/build-newlib.sh) and libstdc++
# (tools/build-libstdcxx.sh), a native host C++ toolchain + cmake + ninja
# (for the required host-tools build - moc/rcc must run on the host, not
# inside PureUnix - see docs/qt-port.md), curl, tar, patch.
set -euo pipefail

QT_VERSION=6.5.3
QT_TARBALL="qtbase-everywhere-src-${QT_VERSION}.tar.xz"
QT_URL="https://download.qt.io/official_releases/qt/6.5/${QT_VERSION}/submodules/${QT_TARBALL}"
QT_SHA256="df2f4a230be4ea04f9798f2c19ab1413a3b8ec6a80bef359f50284235307b546"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# qtbase's own top-level CMakeLists.txt refuses a build directory reached
# through a symlink ("qt_internal_check_if_path_has_symlinks") - on macOS
# /tmp is itself a symlink to /private/tmp, so mktemp -d /tmp/... must be
# resolved to its real physical path (pwd -P) before use, or CMake's own
# configure step hard-errors before anything Qt-specific even runs.
WORK_DIR="$(cd "$(mktemp -d /tmp/pureunix-qt.XXXXXX)" && pwd -P)"
VENDOR_DIR="${REPO_ROOT}/third_party/qt"
NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
LIBSTDCXX_DIR="${REPO_ROOT}/third_party/libstdcxx/i686-elf"

echo "==> Working in ${WORK_DIR}"
command -v i686-elf-g++ >/dev/null || { echo "i686-elf-g++ not found on PATH" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found on PATH (needed for both host and target builds)" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja not found on PATH" >&2; exit 1; }
[ -f "${NEWLIB_DIR}/lib/libc.a" ] || { echo "vendored newlib not found - run tools/build-newlib.sh first" >&2; exit 1; }
[ -f "${LIBSTDCXX_DIR}/lib/libstdc++.a" ] || { echo "vendored libstdc++ not found - run tools/build-libstdcxx.sh first" >&2; exit 1; }

echo "==> Fetching qtbase ${QT_VERSION} source"
curl -Lo "${WORK_DIR}/${QT_TARBALL}" "${QT_URL}"
echo "${QT_SHA256}  ${WORK_DIR}/${QT_TARBALL}" | shasum -a 256 -c -
tar xJf "${WORK_DIR}/${QT_TARBALL}" -C "${WORK_DIR}"
mv "${WORK_DIR}/qtbase-everywhere-src-${QT_VERSION}" "${WORK_DIR}/qtbase"

echo "==> Applying PureUnix patches"
for p in "${VENDOR_DIR}"/patches/*.patch; do
  echo "  ${p}"
  patch -p1 -d "${WORK_DIR}/qtbase" < "${p}"
done

echo "==> Installing PureUnix mkspec"
cp -R "${VENDOR_DIR}/mkspecs/pureunix-g++" "${WORK_DIR}/qtbase/mkspecs/"

echo "==> Building link-probe helper objects (see tools/pureunix-qt-toolchain.cmake's LINKSTUB_DIR comment)"
mkdir -p "${REPO_ROOT}/build/qt-linkstub"
COMMON_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wextra -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -I${REPO_ROOT}/include"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin -isystem "${REPO_ROOT}/user/newlib_compat" -isystem "${NEWLIB_DIR}/include" \
  -c "${REPO_ROOT}/user/newlib_syscalls.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_syscalls.o"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin -isystem "${REPO_ROOT}/user/newlib_compat" -isystem "${NEWLIB_DIR}/include" \
  -c "${REPO_ROOT}/user/newlib_crt0.c" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0.o"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin \
  -c "${REPO_ROOT}/user/newlib_crt0.S" -o "${REPO_ROOT}/build/qt-linkstub/newlib_crt0_asm.o"

# --- Stage 1: native host build, tools only (moc/rcc must run on the host
# that's compiling Qt, never inside PureUnix - see docs/qt-port.md). Image
# codecs disabled: bundled libpng's pngpriv.h hits an unrelated "'fp.h' file
# not found" build bug on this host toolchain when PNG_FLOATING_ARITHMETIC
# gets enabled, and the host build only needs to produce moc/rcc/syncqt,
# not working image codecs.
echo "==> Configuring native host Qt (tools only, for moc/rcc)"
mkdir -p "${WORK_DIR}/host-build" "${WORK_DIR}/host-install"
cmake -G Ninja -S "${WORK_DIR}/qtbase" -B "${WORK_DIR}/host-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${WORK_DIR}/host-install" \
  -DFEATURE_thread=ON \
  -DFEATURE_network=OFF \
  -DFEATURE_sql=OFF \
  -DFEATURE_dbus=OFF \
  -DFEATURE_opengl=OFF \
  -DFEATURE_printsupport=OFF \
  -DQT_FEATURE_accessibility=OFF \
  -DFEATURE_png=OFF \
  -DFEATURE_gif=OFF \
  -DFEATURE_jpeg=OFF \
  -DFEATURE_ico=OFF \
  -DFEATURE_system_libb2=OFF \
  -DQT_BUILD_EXAMPLES=OFF \
  -DQT_BUILD_TESTS=OFF

echo "==> Building native host Qt"
cmake --build "${WORK_DIR}/host-build" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --install "${WORK_DIR}/host-build"

# moc/rcc are host-native binaries (they run on the machine compiling Qt,
# never inside PureUnix - see docs/qt-port.md) - architecture/OS-specific,
# so unlike everything else this repo vendors, they are NOT committed to
# git. They're persisted under build/ (already gitignored, like every
# other build artifact) purely so this multi-minute host build doesn't
# need to be redone for every future Qt program's moc step - the
# Makefile's MOC/RCC variables point here. Re-run this script to refresh
# them for a different host machine/toolchain.
echo "==> Persisting host tools (moc/rcc) to ${REPO_ROOT}/build/qt-host-tools"
rm -rf "${REPO_ROOT}/build/qt-host-tools"
mkdir -p "${REPO_ROOT}/build/qt-host-tools"
cp -R "${WORK_DIR}/host-install/." "${REPO_ROOT}/build/qt-host-tools/"

# --- Stage 2: real cross-compile for PureUnix (i686), using the host
# build's moc/rcc via QT_HOST_PATH. See docs/qt-port.md for the full
# rationale behind every one of these flags/features and every real build
# error each one fixes; only a summary is repeated here:
#   - FEATURE_thread=OFF: no threading exists on PureUnix at all.
#   - FEATURE_network/sql/dbus/printsupport/accessibility=OFF,
#     INPUT_opengl=no: explicitly excluded per the port's own scope
#     (docs/qt-port.md's module list) - INPUT_opengl (not FEATURE_opengl)
#     is required or a later summary-validation step hard-errors anyway.
#   - FEATURE_dlopen=OFF: no dynamic loading exists on PureUnix at all;
#     this also disables QT_FEATURE_library (QLibrary/QPluginLoader),
#     which otherwise pulls in a real <dlfcn.h> dependency we don't have.
#   - FEATURE_testlib=OFF: QtTestLib isn't part of this port's scope
#     (Core/Gui/Widgets only).
#   - FEATURE_reduce_relocations=OFF: this feature (on by default for
#     ELF+GCC Unix targets) requires every *consumer* of Qt to also
#     compile with -fPIC, hard-erroring via qcompilerdetection.h's
#     QT_REDUCE_RELOCATIONS check otherwise. PureUnix has no PIC/PIE
#     support anywhere (fixed-address ET_EXEC only, see docs/qt-port.md) -
#     discovered when the very first real Qt Core consumer program
#     (user/qtcoretest.cpp, Phase 4) failed to compile against an
#     otherwise-successful Phase 3 Qt build.
#   - FEATURE_system_libb2=OFF: qtbase's find_package(Libb2) uses
#     pkg-config, which (unlike CMAKE_FIND_ROOT_PATH_MODE_LIBRARY/INCLUDE)
#     isn't sysroot-scoped here and happily reports the *host* machine's
#     libb2 (if installed) as found - satisfying the CMake-level check
#     while producing a target build with zero real blake2 implementation
#     linked in anywhere (undefined blake2b_init/blake2s_init/etc at real
#     executable link time - only caught in Phase 4, since Phase 3 only
#     ever exercised STATIC_LIBRARY try_compiles, never a real full link).
#     Forcing this off makes qtbase compile its own bundled
#     src/3rdparty/blake2 reference implementation instead, the same
#     "use the bundled copy, not an unreliable system one" treatment
#     already applied to zlib/libpng/libjpeg/freetype/harfbuzz/pcre2.
# tools/pureunix-qt-toolchain.cmake carries the rest: the compiler/flags/include
# paths, CMAKE_SYSTEM_NAME=Linux (a pragmatic proxy purely to make CMake's
# own UNIX variable true, not a real OS claim - see that file's comment),
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY plus the one real linked
# config test that still needs -nostartfiles/crt0/linker.ld, and the
# handful of compiler defines (-DPCRE2_DISABLE_JIT, -DHB_NO_MT,
# -D__PUREUNIX__, -U__FLT16_MAX__, -D_GNU_SOURCE) and flags
# (-Wno-incompatible-pointer-types, -fpermissive) each fixing one specific,
# documented real build error.
echo "==> Configuring cross-compiled Qt for PureUnix"
mkdir -p "${WORK_DIR}/target-build" "${WORK_DIR}/target-install"
cmake -G Ninja -S "${WORK_DIR}/qtbase" -B "${WORK_DIR}/target-build" \
  -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/pureunix-qt-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${WORK_DIR}/target-install" \
  -DQT_QMAKE_TARGET_MKSPEC=pureunix-g++ \
  -DQT_HOST_PATH="${WORK_DIR}/host-install" \
  -DFEATURE_thread=OFF \
  -DFEATURE_network=OFF \
  -DFEATURE_sql=OFF \
  -DFEATURE_dbus=OFF \
  -DINPUT_opengl=no \
  -DFEATURE_printsupport=OFF \
  -DQT_FEATURE_accessibility=OFF \
  -DFEATURE_dlopen=OFF \
  -DFEATURE_testlib=OFF \
  -DFEATURE_reduce_relocations=OFF \
  -DFEATURE_system_libb2=OFF \
  -DQT_BUILD_EXAMPLES=OFF \
  -DQT_BUILD_TESTS=OFF

echo "==> Building cross-compiled Qt for PureUnix (Core + Gui + Widgets)"
cmake --build "${WORK_DIR}/target-build" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --install "${WORK_DIR}/target-build"

echo "==> Vendoring into ${VENDOR_DIR}/i686-elf"
rm -rf "${VENDOR_DIR}/i686-elf"
mkdir -p "${VENDOR_DIR}/i686-elf"
cp -R "${WORK_DIR}/target-install/include" "${VENDOR_DIR}/i686-elf/"
cp -R "${WORK_DIR}/target-install/lib" "${VENDOR_DIR}/i686-elf/"
cp -R "${WORK_DIR}/target-install/plugins" "${VENDOR_DIR}/i686-elf/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
echo "==> Verify: i686-elf-nm ${VENDOR_DIR}/i686-elf/lib/libQt6Core.a | grep QObject"
