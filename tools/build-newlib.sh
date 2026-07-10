#!/usr/bin/env bash
# Rebuilds third_party/newlib/i686-elf from upstream newlib source.
#
# PureUNIX vendors a prebuilt newlib (headers + libc.a/libm.a) under
# third_party/newlib/ so `make` never needs network access. Run this script
# only when you actually want to regenerate that vendored copy (e.g. to pick
# up a newer newlib release or change build flags).
#
# Requires the i686-elf cross compiler already used to build PureUNIX itself
# (CROSS ?= i686-elf- in the top-level Makefile) to be on PATH, plus curl,
# tar, and a working autotools-generated `configure` (shipped in the newlib
# tarball, so no autoreconf is needed).
set -euo pipefail

NEWLIB_VERSION=4.6.0.20260123
NEWLIB_TARBALL="newlib-${NEWLIB_VERSION}.tar.gz"
NEWLIB_URL="https://sourceware.org/pub/newlib/${NEWLIB_TARBALL}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="$(mktemp -d /tmp/pureunix-newlib.XXXXXX)"
INSTALL_DIR="${WORK_DIR}/install"
VENDOR_DIR="${REPO_ROOT}/third_party/newlib"

echo "==> Working in ${WORK_DIR}"
command -v i686-elf-gcc >/dev/null || { echo "i686-elf-gcc not found on PATH" >&2; exit 1; }

echo "==> Fetching newlib ${NEWLIB_VERSION}"
curl -Lo "${WORK_DIR}/${NEWLIB_TARBALL}" "${NEWLIB_URL}"
tar xzf "${WORK_DIR}/${NEWLIB_TARBALL}" -C "${WORK_DIR}"

mkdir -p "${WORK_DIR}/build" "${INSTALL_DIR}"
cd "${WORK_DIR}/build"

# --target=i686-elf matches the existing bare-metal cross compiler exactly
# (newlib has no case for i686-*-elf in configure.host, so it never builds
# libc/syscalls' POSIX-named wrappers on its own). -DMISSING_SYSCALL_NAMES
# makes the libc/reent/*.c layer call the plain POSIX names (open, read,
# write, ...) instead of the underscored ones (_open, _read, ...); PureUNIX
# supplies exactly those plain names in user/newlib_syscalls.c.
# Deliberately NOT using --enable-newlib-nano-formatted-io: that smaller
# printf/scanf variant has a latent bug in this newlib release where
# nano-vfprintf.c unconditionally calls _fputwc_r, but the wide-char stdio
# object that defines it is never compiled in for the nano-IO configuration
# — every newlib-linked binary fails to link. The standard vfprintf/vfscanf
# used here are fully self-contained (and support %f natively, unlike nano
# IO which needs an extra `-Wl,-u,_printf_float`) at the cost of a larger
# libc.a; plenty of room in the 3 MiB per-process window for that.
#
# --enable-newlib-io-long-long: BusyBox's procps/top.c parses
# /proc/stat's jiffy counters with sscanf("...%llu%llu...", ...) — without
# this, newlib's vfscanf silently can't match %llu (this was found by
# top failing "can't read '/proc/stat'" even though the file's content and
# a plain fopen()/fgets() against it were both already verified correct;
# root cause was this newlib build flag, not procfs).
"${WORK_DIR}"/newlib-${NEWLIB_VERSION}/configure \
  --target=i686-elf \
  --prefix="${INSTALL_DIR}" \
  --disable-newlib-multithread \
  --disable-nls \
  --disable-multilib \
  --enable-newlib-io-long-long \
  CFLAGS_FOR_TARGET="-ffreestanding -O2 -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -DMISSING_SYSCALL_NAMES"

# Only the newlib subtree is built/installed — libgloss (board support
# packages for real hardware/simulators we don't use) needs `makeinfo` for
# its docs and isn't needed here; skipping it avoids that dependency.
make all-target-newlib -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make -C i686-elf/newlib install

rm -f "${INSTALL_DIR}/i686-elf/lib/libg.a"  # duplicate of libc.a, not needed

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}/i686-elf"
mkdir -p "${VENDOR_DIR}/i686-elf"
cp -R "${INSTALL_DIR}/i686-elf/include" "${VENDOR_DIR}/i686-elf/"
cp -R "${INSTALL_DIR}/i686-elf/lib" "${VENDOR_DIR}/i686-elf/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
