#!/usr/bin/env bash
# Rebuilds third_party/busybox/busybox.elf from upstream BusyBox source.
#
# PureUNIX vendors a prebuilt BusyBox binary (busybox.elf) under
# third_party/busybox/ so `make` never needs network access or a copy of
# BusyBox's ~4400-file source tree in this repo — the same "vendor a
# prebuilt artifact, not the source" choice third_party/newlib/ makes (see
# tools/build-newlib.sh). Run this script only when you actually want to
# regenerate that vendored copy (e.g. to pick a different applet set, bump
# the BusyBox version, or pick up a PureUNIX libc/syscall change).
#
# Requires the i686-elf cross compiler already used to build PureUNIX
# itself (CROSS ?= i686-elf- in the top-level Makefile) to be on PATH,
# plus curl and tar. BusyBox's own Kbuild handles compiling every applet
# object file; this script only takes over for the *final link*, since
# Kbuild's own link step assumes a hosted target with real crt0.o/crt1.o
# startup files, which this freestanding target doesn't have (see
# user/newlib_crt0.S, user/crt0.S).
set -euo pipefail

BUSYBOX_VERSION=1.36.1
BUSYBOX_TARBALL="busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="$(mktemp -d /tmp/pureunix-busybox.XXXXXX)"
VENDOR_DIR="${REPO_ROOT}/third_party/busybox"
NEWLIB_DIR="${REPO_ROOT}/third_party/newlib/i686-elf"
COMPAT_DIR="${REPO_ROOT}/user/newlib_compat"
BUILD_USER_DIR="${REPO_ROOT}/build/user"

echo "==> Working in ${WORK_DIR}"
command -v i686-elf-gcc >/dev/null || { echo "i686-elf-gcc not found on PATH" >&2; exit 1; }
for obj in newlib_crt0_asm.o newlib_crt0.o newlib_syscalls.o; do
  [ -f "${BUILD_USER_DIR}/${obj}" ] || {
    echo "missing ${BUILD_USER_DIR}/${obj} — run 'make' in the repo root first" >&2
    exit 1
  }
done

echo "==> Fetching BusyBox ${BUSYBOX_VERSION}"
curl -Lo "${WORK_DIR}/${BUSYBOX_TARBALL}" "${BUSYBOX_URL}"
tar xjf "${WORK_DIR}/${BUSYBOX_TARBALL}" -C "${WORK_DIR}"
cd "${WORK_DIR}/busybox-${BUSYBOX_VERSION}"

echo "==> Applying pureunix.config"
cp "${VENDOR_DIR}/pureunix.config" .config

EXTRA_CFLAGS="-isystem ${COMPAT_DIR} -isystem ${NEWLIB_DIR}/include -ffreestanding -fno-stack-protector -fno-pic -fno-pie -D_GNU_SOURCE"

# oldconfig (not olddefconfig) so a version bump's new config options get
# their real prompts on stderr instead of silently taking a default that
# might re-enable something we deliberately turned off (e.g. a shell).
# `|| true`: oldconfig itself always finishes correctly and updates .config,
# but it can close its stdin before `yes` is done writing, so the pipe's own
# exit status is racy (SIGPIPE) under `set -o pipefail` and must not be
# treated as this script's own failure.
yes "" | make oldconfig ARCH=i386 CROSS_COMPILE=i686-elf- >/dev/null || true

echo "==> Compiling (BusyBox's own Kbuild — every applet .o/.a file)"
# The final link (busybox_unstripped) is expected to fail here: BusyBox's
# own link recipe assumes a hosted target with real crt0.o/crt1.o, which
# this freestanding cross toolchain doesn't provide. Kbuild still leaves
# every object file/archive it built, which is all this script needs next.
make -k ARCH=i386 CROSS_COMPILE=i686-elf- EXTRA_CFLAGS="${EXTRA_CFLAGS}" || true

[ -f applets/built-in.o ] || { echo "BusyBox object files weren't produced — a real compile error occurred above" >&2; exit 1; }

echo "==> Linking busybox.elf against PureUNIX's own crt0/newlib glue"
# Same objects/archives BusyBox's own scripts/trylink prints (see
# docs/userland.md's BusyBox section) — recompute by hand rather than
# scraping trylink's output, since a version bump can add or remove
# subdirectories. --gc-sections drops every applet/libbb function nothing
# in pureunix.config's enabled applet set actually calls (BusyBox's own
# link would do this too, but its --gc-sections detection fails against
# this cross toolchain's binutils for unrelated reasons).
# `mapfile`/`readarray` need bash >= 4; macOS ships 3.2, so build the array
# the portable way instead (safe here — Kbuild's own object/archive paths
# never contain spaces).
# maxdepth 3, not 2: some applet groups keep a small helper library in a
# nested subdirectory (e.g. coreutils/libcoreutils/lib.a, one level deeper
# than coreutils/built-in.o itself) that Kbuild deliberately does NOT fold
# into the parent directory's own built-in.o — --gc-sections below still
# drops anything nothing enabled actually calls.
LINK_OBJS=()
while IFS= read -r obj; do
  LINK_OBJS+=("${obj}")
done < <(find . -maxdepth 3 \( -name 'built-in.o' -o -name 'lib.a' \) | sort)

i686-elf-gcc -T "${REPO_ROOT}/user/linker.ld" -ffreestanding -nostdlib -Wl,--build-id=none -Wl,--gc-sections \
  "${BUILD_USER_DIR}/newlib_crt0_asm.o" "${BUILD_USER_DIR}/newlib_crt0.o" "${BUILD_USER_DIR}/newlib_syscalls.o" \
  -Wl,--start-group "${LINK_OBJS[@]}" -Wl,--end-group \
  -L"${NEWLIB_DIR}/lib" -Wl,--start-group -lc -lm -Wl,--end-group -lgcc \
  -o busybox.elf

echo "==> Vendoring into ${VENDOR_DIR}"
cp busybox.elf "${VENDOR_DIR}/busybox.elf"
cp .config "${VENDOR_DIR}/pureunix.config"

echo "==> Done. Review changes under ${VENDOR_DIR} and commit them."
