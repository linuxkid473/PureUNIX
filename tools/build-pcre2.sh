#!/usr/bin/env bash
# Builds third_party/pcre2/i686-elf (headers + libpcre2-8.a) from the
# vendored upstream source under third_party/pcre2/pcre2-10.47/.
#
# Second new dependency for the PCManFM-Qt port (docs/pcmanfm-port.md
# phase 5) — GLib's GRegex needs a real PCRE2. Qt's own build already
# vendors a cross-built PCRE2 (third_party/qt/i686-elf/lib/
# libQt6BundledPcre2.a), but that's an internal Qt CMake build product,
# not a standalone reusable libpcre2-8.a with its own public headers in
# a form GLib's own build can just find — simplest, most isolated path is
# a second, independent PCRE2 build specifically for GLib, same version,
# not sharing a build artifact across two unrelated build systems.
#
# Same vendoring pattern as every other out-of-tree dependency in this
# repo (tools/build-ncurses.sh/build-libffi.sh): run once, offline,
# cross-compiling with the same i686-elf-gcc and newlib every other
# PureUNIX userspace binary uses, and commit the resulting headers +
# static lib so `make`/`make iso` stay network-free.
#
# JIT deliberately NOT enabled (upstream's own --enable-jit is off by
# default, so simply never passing it is enough — no patch needed): JIT
# requires real pthreads (configure.ac's own AX_PTHREAD check, gated
# behind --enable-jit) for its internal executable-code-buffer pool.
# PureUnix now has a real (if deliberately single-threaded, see
# user/newlib_syscalls.c's own comment) pthread implementation, but JIT
# is a pure performance optimization PCRE2 works correctly without —
# same reasoning Qt's own build already used (-DPCRE2_DISABLE_JIT,
# docs/qt-port.md), not re-litigated here since nothing about this port
# needs JIT-compiled regex matching. Only the core library is built here
# (not pcre2grep/pcre2test, which need zlib/bzlib/readline this port has
# no use for) — GLib only ever links libpcre2-8.a itself.
set -euo pipefail

PCRE2_VERSION=10.47

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PCRE2_SRC="${REPO_ROOT}/third_party/pcre2/pcre2-${PCRE2_VERSION}"
VENDOR_DIR="${REPO_ROOT}/third_party/pcre2/i686-elf"
CROSS="${CROSS:-i686-elf-}"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found on PATH" >&2; exit 1; }
[ -d "${PCRE2_SRC}" ] || { echo "missing ${PCRE2_SRC} — vendor the pcre2-${PCRE2_VERSION} source first" >&2; exit 1; }

echo "==> Building newlib_crt0/newlib_syscalls prerequisites"
make -C "${REPO_ROOT}" build/user/newlib_crt0_asm.o build/user/newlib_crt0.o build/user/newlib_syscalls.o

WORK_DIR="$(mktemp -d /tmp/pureunix-pcre2.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
echo "==> Working in ${WORK_DIR}"

echo "==> Copying vendored source (pristine third_party/pcre2/pcre2-${PCRE2_VERSION}/ is never modified in place)"
cp -R "${PCRE2_SRC}" "${WORK_DIR}/src"

NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
CRT0_OBJS="${REPO_ROOT}/build/user/newlib_crt0_asm.o ${REPO_ROOT}/build/user/newlib_crt0.o ${REPO_ROOT}/build/user/newlib_syscalls.o"

# Matches USER_CFLAGS + NEWLIB_CFLAGS in the top-level Makefile exactly, so
# the resulting libpcre2-8.a is flag-for-flag consistent with every other
# newlib-linked PureUNIX binary it gets linked into.
# -Wno-incompatible-pointer-types: this newlib target's int32_t is `long`
# (not plain `int`) — a real, already-documented fact from the Qt6 port
# (docs/qt-port.md: "int32_t-is-long on this newlib target"), not a bug.
# Both are genuinely 32-bit and layout-compatible on i686; PCRE2's own
# code (like a lot of C code written assuming int32_t==int) passes an
# `int32_t *` where a function prototype says `int *`, which is only a
# strict-type-matching diagnostic here, never a real ABI mismatch.
TARGET_CFLAGS="-std=gnu99 -ffreestanding -O2 -Wall -Wno-unused-parameter -Wno-incompatible-pointer-types -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -fno-builtin"
TARGET_CPPFLAGS="-isystem ${REPO_ROOT}/user/newlib_compat -isystem ${NEWLIB_DIR}/include"
# -T user/linker.ld: see tools/build-libffi.sh's own comment/
# gotcha_libffi_cross_build_linker_ld_libtool.md — newlib_crt0.c
# unconditionally references TLS section-boundary symbols only defined
# there, needed for configure's own bootstrap "does the compiler work"
# link check to succeed at all.
TARGET_LDFLAGS="-T ${REPO_ROOT}/user/linker.ld -nostdlib -L${NEWLIB_DIR}/lib"
TARGET_LIBS="${CRT0_OBJS} -lc -lm -lgcc"

BUILD_TRIPLE="$("${WORK_DIR}/src/config.guess" 2>/dev/null || echo unknown)"
if [ "${BUILD_TRIPLE}" = "unknown" ]; then
	# PCRE2's own config.guess (not vendored in every release tarball the
	# way libffi's is) — fall back to the host's own autoconf-provided copy.
	BUILD_TRIPLE="$(/usr/bin/env config.guess 2>/dev/null || uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
fi

mkdir -p "${WORK_DIR}/build"
cd "${WORK_DIR}/build"

echo "==> Configuring PCRE2 ${PCRE2_VERSION} for i686-elf (cross, JIT off — see this script's own comment)"
CC="${CROSS}gcc" \
CPPFLAGS="${TARGET_CPPFLAGS}" \
CFLAGS="${TARGET_CFLAGS}" \
LDFLAGS="${TARGET_LDFLAGS}" \
LIBS="${TARGET_LIBS}" \
"${WORK_DIR}/src/configure" \
	--host=i686-elf \
	--build="${BUILD_TRIPLE}" \
	--disable-shared \
	--disable-pcre2grep-libz \
	--disable-pcre2grep-libbz2 \
	--disable-pcre2test-libreadline \
	--disable-pcre2test-libedit \
	--enable-pcre2-8 \
	--disable-pcre2-16 \
	--disable-pcre2-32 \
	--prefix=/usr

echo "==> Building the core library only (real per-file compiles; may hit"
echo "    libtool's own final-archive-link limitation, same as"
echo "    tools/build-libffi.sh — checked for specifically below, not"
echo "    blanket-ignored)"
BUILD_LOG="${WORK_DIR}/build.log"
if make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" libpcre2-8.la 2>&1 | tee "${BUILD_LOG}"; then
	MAKE_OK=1
else
	MAKE_OK=0
fi

if [ "${MAKE_OK}" -eq 0 ]; then
	# Only the exact known-tolerable failure (see tools/build-libffi.sh's
	# own comment/gotcha_libffi_cross_build_linker_ld_libtool.md) is safe
	# to proceed past: libtool refusing its own final .la archive step
	# because the bootstrap-probe's crt0/newlib_syscalls .o files are
	# present in the global LIBS var. Any *other* real compile/link error
	# (e.g. a genuine source incompatibility) must NOT be silently papered
	# over — grep for real per-file compile failures distinct from that
	# one known, tolerable message.
	if ! grep -q "cannot build libtool library" "${BUILD_LOG}"; then
		echo "make failed for a reason other than the known libtool-archival limitation — real build failure, see ${BUILD_LOG} above" >&2
		exit 1
	fi
	REAL_ERRORS=$(grep -E '(^|[^a-zA-Z])error:' "${BUILD_LOG}" | grep -vc "cannot build libtool library" || true)
	if [ "${REAL_ERRORS}" -gt 0 ]; then
		echo "make failed with ${REAL_ERRORS} real compile error(s) in addition to the known libtool-archival limitation — see ${BUILD_LOG} above" >&2
		exit 1
	fi
	echo "==> make's only failure was the known libtool-archival limitation; every real .o compiled fine"
fi

LIB_OBJS=$(find src -maxdepth 1 -name '*.o')
if [ -z "${LIB_OBJS}" ]; then
	echo "no libpcre2-8 object files were produced — real build failure, see output above" >&2
	exit 1
fi

echo "==> Archiving libpcre2-8.a directly (bypassing libtool's own final link if it failed)"
if [ ! -f .libs/libpcre2-8.a ]; then
	"${CROSS}ar" rcs libpcre2-8.a ${LIB_OBJS}
	"${CROSS}ranlib" libpcre2-8.a
	LIBFILE="libpcre2-8.a"
else
	LIBFILE=".libs/libpcre2-8.a"
fi

echo "==> Vendoring into ${VENDOR_DIR}"
rm -rf "${VENDOR_DIR}"
mkdir -p "${VENDOR_DIR}/include" "${VENDOR_DIR}/lib"
cp src/pcre2.h "${VENDOR_DIR}/include/"
cp "${LIBFILE}" "${VENDOR_DIR}/lib/libpcre2-8.a"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
