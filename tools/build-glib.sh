#!/usr/bin/env bash
# Builds third_party/glib/i686-elf (headers + libglib-2.0.a/libgobject-2.0.a/
# libgio-2.0.a) from the vendored upstream source under
# third_party/glib/glib-2.88.2/.
#
# The big one: docs/pcmanfm-port.md phase 3/6 — GLib/GObject/GIO is
# libfm-qt's actual VFS backend in every version, not optional/patchable
# (see that doc's own research). Real, honest single-threaded pthread
# shim (user/newlib_syscalls.c), real iconv() (same file), libffi
# (third_party/libffi) and PCRE2 (third_party/pcre2) are all real
# prerequisites already landed before this script can even attempt to
# configure.
#
# Meson (not autotools — GLib dropped it years ago), cross-compiling via
# tools/pureunix-glib-crossfile.ini (a template — @REPO_ROOT@ substituted
# below with this repo's real absolute path, see that file's own
# comment).
#
# Scope: local files only. No D-Bus wiring needed at build time (GLib's
# own GDBus is a from-scratch reimplementation over plain sockets, not a
# libdbus binding — real finding, checked against meson.build, not
# assumed: no 'dbus' build option/dependency exists at all). No
# GFileMonitor native backend (file_monitor_backend 'auto' only resolves
# to inotify/kqueue/win32, none of which apply to a `system = 'none'`
# cross target — real, checked in gio/meson.build, not a patch: every
# `if file_monitor_backend == '...'` block simply never matches, so no
# backend gets built and gio/gfile.c's own generic fallback governs
# GFileMonitor's behavior instead). No GVolumeMonitor beyond whatever
# gracefully degrades with no real backend either. introspection/tests/
# documentation/man-pages/nls disabled — none needed for a PCManFM-Qt
# runtime dependency.
set -euo pipefail

GLIB_VERSION=2.88.2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
GLIB_SRC="${REPO_ROOT}/third_party/glib/glib-${GLIB_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/glib/i686-elf"
CROSSFILE_TEMPLATE="${SCRIPT_DIR}/pureunix-glib-crossfile.ini"

command -v i686-elf-gcc >/dev/null || { echo "i686-elf-gcc not found on PATH" >&2; exit 1; }
command -v meson >/dev/null || { echo "meson not found on PATH (brew install meson)" >&2; exit 1; }
[ -d "${GLIB_SRC}" ] || { echo "missing ${GLIB_SRC} — vendor the glib-${GLIB_VERSION} source first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-glib.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/glib/glib-${GLIB_VERSION}/ is never modified in place)"
cp -R "${GLIB_SRC}" "${WORK_DIR}/src"

echo "==> Applying PureUnix patches (see third_party/glib/patches/)"
for p in "${REPO_ROOT}/third_party/glib/patches"/*.patch; do
	echo "    ${p}"
	patch -p1 -d "${WORK_DIR}/src" < "${p}"
done

echo "==> Generating the real cross file (substituting ${REPO_ROOT})"
CROSSFILE="${WORK_DIR}/crossfile.ini"
sed "s|@REPO_ROOT@|${REPO_ROOT}|g" "${CROSSFILE_TEMPLATE}" > "${CROSSFILE}"

# libffi/pcre2 are found via pkg-config (third_party/{libffi,pcre2}/i686-elf/
# lib/pkgconfig/*.pc, written by hand since neither vendored build
# produces one — see those scripts' own comments) — point pkg-config's
# own search path at exactly those two .pc files, nothing from the host
# system's own pkg-config database (PKG_CONFIG_LIBDIR replaces the
# default search path entirely, rather than PKG_CONFIG_PATH which only
# adds to it — deliberate: a host libffi.pc/libpcre2-8.pc must never be
# picked up by accident here).
export PKG_CONFIG_LIBDIR="${REPO_ROOT}/third_party/libffi/i686-elf/lib/pkgconfig:${REPO_ROOT}/third_party/pcre2/i686-elf/lib/pkgconfig"

echo "==> Configuring GLib ${GLIB_VERSION} for i686 PureUnix (cross, Meson)"
BUILD_DIR="${WORK_DIR}/build"
meson setup "${BUILD_DIR}" "${WORK_DIR}/src" \
	--cross-file "${CROSSFILE}" \
	--default-library=static \
	--buildtype=release \
	--prefix=/usr \
	-Dselinux=disabled \
	-Dlibmount=disabled \
	-Dxattr=false \
	-Dman-pages=disabled \
	-Ddtrace=disabled \
	-Dsystemtap=disabled \
	-Dsysprof=disabled \
	-Ddocumentation=false \
	-Dtests=false \
	-Dinstalled_tests=false \
	-Dnls=disabled \
	-Dintrospection=disabled \
	-Dlibelf=disabled \
	-Dmultiarch=false \
	-Dglib_debug=disabled \
	-Doss_fuzz=disabled

echo "==> Building (this will take a while)"
ninja -C "${BUILD_DIR}"

echo "==> Installing into a scratch DESTDIR"
INSTALL_ROOT="${WORK_DIR}/install"
DESTDIR="${INSTALL_ROOT}" meson install -C "${BUILD_DIR}"

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}"
cp -R "${INSTALL_ROOT}/usr/local/include" "${VENDOR_DIR}/include" 2>/dev/null || \
	cp -R "${INSTALL_ROOT}/usr/include" "${VENDOR_DIR}/include"
cp -R "${INSTALL_ROOT}/usr/local/lib" "${VENDOR_DIR}/lib" 2>/dev/null || \
	cp -R "${INSTALL_ROOT}/usr/lib" "${VENDOR_DIR}/lib"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
