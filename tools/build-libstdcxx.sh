#!/usr/bin/env bash
# Builds third_party/libstdcxx/i686-elf from upstream GCC's libstdc++-v3
# source, cross-compiled against the already-vendored newlib
# (third_party/newlib/i686-elf).
#
# PureUNIX's i686-elf-g++ (Homebrew, GCC 16.1.0) was built --without-headers
# as a bootstrap-only compiler: it never built libstdc++ because no target
# libc existed at GCC-build time. This script builds libstdc++-v3 out of
# tree, standalone, the same way tools/build-newlib.sh vendors a prebuilt
# newlib: run once, offline, and commit the resulting headers + libstdc++.a
# / libsupc++.a so `make` never needs network access or a source GCC tree.
#
# Requires: the same i686-elf-g++ used to build PureUNIX itself, curl, tar.
# The GCC source version MUST match the installed compiler's version
# (16.1.0) — libstdc++-v3 is tightly coupled to its own compiler's ABI/
# builtins and libgcc.
#
# Three non-obvious fixes were needed to get here (see docs/qt-port.md):
#
# 1. i686-elf-gcc has no start files (crt0.o/crti.o/crtn.o) — it was built
#    --without-headers, so libstdc++-v3 configure's very first "does the
#    compiler work" link probe fails with undefined-reference-to-_start-ish
#    errors, which autoconf's GCC_NO_EXECUTABLES machinery then latches into
#    a permanent "no link tests allowed" mode that hard-errors on later
#    AC_CHECK_FUNCS calls it can't gracefully degrade. Fix: -nostartfiles.
# 2. Even with -nostartfiles, newlib's own libc.a is built with
#    -DMISSING_SYSCALL_NAMES (see tools/build-newlib.sh) — its reentrant
#    layer calls bare POSIX names (close, read, write, ...) that only
#    user/newlib_syscalls.c actually defines. So the initial link probe
#    still fails until that real object (plus the newlib crt0 objects that
#    define _start_c/environ handling) is linked in too — exactly the set
#    every real PureUnix newlib program already links against.
# 3. i686-elf-gcc reports "Thread model: single" (no gthreads backend was
#    ever selected during its own bootstrap build) — libstdc++-v3 correctly
#    picks the single-threaded gthr-single.h backend automatically; no
#    pthread support is needed for libstdc++ itself (PureUnix has none -
#    see docs/qt-port.md section 3). This just falls out of the existing
#    compiler config; nothing to configure explicitly.
#
# -ffreestanding is deliberately NOT used for this build (unlike the rest of
# PureUnix's userland): it makes GCC predefine __STDC_HOSTED__=0, which
# libstdc++-v3 headers read to decide whether to expose the real ("hosted")
# library at all (streams, real allocators, __throw_bad_alloc, etc.) —
# passing it silently drops down to a crippled freestanding-only subset,
# which is not what a real Qt-capable std::string/vector/etc needs. PureUnix
# genuinely is hosted here: newlib provides a real libc with real syscalls.
set -euo pipefail

GCC_VERSION=16.1.0
GCC_TARBALL="gcc-${GCC_VERSION}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/${GCC_TARBALL}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="$(mktemp -d /tmp/pureunix-libstdcxx.XXXXXX)"
INSTALL_DIR="${WORK_DIR}/install"
VENDOR_DIR="${REPO_ROOT}/third_party/libstdcxx"
NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
COMPAT_DIR="${REPO_ROOT}/user/newlib_compat"

echo "==> Working in ${WORK_DIR}"
command -v i686-elf-g++ >/dev/null || { echo "i686-elf-g++ not found on PATH" >&2; exit 1; }
INSTALLED_VER="$(i686-elf-g++ -dumpversion)"
if [ "${INSTALLED_VER}" != "${GCC_VERSION}" ]; then
  echo "installed i686-elf-g++ is ${INSTALLED_VER}, this script is pinned to ${GCC_VERSION} - update GCC_VERSION or the toolchain" >&2
  exit 1
fi
[ -f "${NEWLIB_DIR}/lib/libc.a" ] || { echo "vendored newlib not found at ${NEWLIB_DIR} - run tools/build-newlib.sh first" >&2; exit 1; }

echo "==> Fetching gcc ${GCC_VERSION} source (for libstdc++-v3 only)"
curl -Lo "${WORK_DIR}/${GCC_TARBALL}" "${GCC_URL}"
tar xJf "${WORK_DIR}/${GCC_TARBALL}" -C "${WORK_DIR}"

echo "==> Building link-probe helper objects (see fix #2 above)"
STUB_DIR="${WORK_DIR}/linkstub"
mkdir -p "${STUB_DIR}"
COMMON_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wextra -Wno-unused-parameter -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -I${REPO_ROOT}/include"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin -isystem "${COMPAT_DIR}" -isystem "${NEWLIB_DIR}/include" \
  -c "${REPO_ROOT}/user/newlib_syscalls.c" -o "${STUB_DIR}/newlib_syscalls.o"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin -isystem "${COMPAT_DIR}" -isystem "${NEWLIB_DIR}/include" \
  -c "${REPO_ROOT}/user/newlib_crt0.c" -o "${STUB_DIR}/newlib_crt0.o"
i686-elf-gcc ${COMMON_CFLAGS} -fno-builtin \
  -c "${REPO_ROOT}/user/newlib_crt0.S" -o "${STUB_DIR}/newlib_crt0_asm.o"

mkdir -p "${WORK_DIR}/build" "${INSTALL_DIR}"
cd "${WORK_DIR}/build"

export CC=i686-elf-gcc
export CXX=i686-elf-g++
TARGET_FLAGS="-O2 -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -isystem ${COMPAT_DIR} -isystem ${NEWLIB_DIR}/include"
export CFLAGS_FOR_TARGET="${TARGET_FLAGS}"
export CXXFLAGS_FOR_TARGET="${TARGET_FLAGS}"
export CPPFLAGS_FOR_TARGET="-isystem ${COMPAT_DIR} -isystem ${NEWLIB_DIR}/include"
export CFLAGS="${TARGET_FLAGS}"
export CXXFLAGS="${TARGET_FLAGS}"
# LDFLAGS here is configure-time only (fix #1+#2): it must include the link
# probe helper objects so libstdc++-v3's initial "does this compiler work"
# check succeeds. The actual `make`/`make install` invocations below
# override LDFLAGS back down to just -nostartfiles -L..., since passing
# plain .o files through LDFLAGS during the real build breaks libtool's
# convenience-archive linking step (it rejects "non-libtool objects").
export LDFLAGS="-nostartfiles -L${NEWLIB_DIR}/lib ${STUB_DIR}/newlib_crt0_asm.o ${STUB_DIR}/newlib_crt0.o ${STUB_DIR}/newlib_syscalls.o"

# --host=i686-elf: from libstdc++-v3's own configure's point of view, "host"
# means "the machine libstdc++ will run on" (matches newlib's --target
# meaning the same thing). --with-newlib tells its configure to assume a
# newlib-shaped libc (no full POSIX, no dynamic loading) rather than glibc.
# --disable-libstdcxx-threads: no real threading exists anywhere on
# PureUnix (see docs/qt-port.md section 3) - Qt's own QMutex/QThread
# wrappers need a separate userland pthread-stub shim, not this library's
# concern (libstdc++ itself already picks single-threaded gthr, see fix #3).
# --disable-libstdcxx-verbose drops the -verbose terminate handler's stderr
# message formatting. --disable-shared/--enable-static: PureUnix has no
# dynamic linking at all (docs/qt-port.md section 2) - only .a archives
# make sense here. --disable-libstdcxx-pch: the precompiled <bits/stdc++.h>
# header is a pure build-time convenience, unrelated to the actual runtime
# library, and hits an unrelated -Wtemplate-body/two-phase-lookup diagnostic
# in this GCC version building bitset's string constructor - just skip it.
# Dual ABI (cow string vs C++11 string) is deliberately left at its default
# (enabled) rather than forced to a single ABI: forcing
# --disable-libstdcxx-dual-abi triggered a real "__getline<>...does not
# match any template declaration" compile error unrelated to ABI selection
# (see fix below) that the far-better-tested default configuration doesn't
# hit; not worth fighting for a config this port doesn't need to slim down.
"${WORK_DIR}/gcc-${GCC_VERSION}/libstdc++-v3/configure" \
  --host=i686-elf \
  --prefix="${INSTALL_DIR}" \
  --disable-multilib \
  --disable-nls \
  --with-newlib \
  --disable-libstdcxx-threads \
  --disable-libstdcxx-verbose \
  --disable-shared \
  --enable-static \
  --disable-libstdcxx-pch

# make/make install override LDFLAGS on the command line (highest
# precedence over the Makefile's own configure-substituted value) back down
# to just the real link flags, dropping the link-probe-only stub objects -
# see the LDFLAGS comment above.
make LDFLAGS="-nostartfiles -L${NEWLIB_DIR}/lib" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make LDFLAGS="-nostartfiles -L${NEWLIB_DIR}/lib" install

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}/i686-elf"
mkdir -p "${VENDOR_DIR}/i686-elf"
cp -R "${INSTALL_DIR}/include" "${VENDOR_DIR}/i686-elf/"
cp -R "${INSTALL_DIR}/lib" "${VENDOR_DIR}/i686-elf/"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
echo "==> Note: user/newlib_compat/stdio.h's 'getline' shim must stay guarded"
echo "    with #ifndef __cplusplus - see that file's header comment - or"
echo "    this build silently reproduces the __getline<> template mismatch."
