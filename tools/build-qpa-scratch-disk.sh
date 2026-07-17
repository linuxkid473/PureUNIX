#!/usr/bin/env bash
# tools/build-qpa-scratch-disk.sh — builds a small, standalone ISO
# (build/qpa-scratch.iso) carrying ONLY BusyBox + user/qtwindowtest.elf on
# its EXT2 root, for interactively verifying the "pureunix" QPA plugin
# (docs/qt-port.md Phase 6, user/qpa_pureunix/) in QEMU.
#
# Why this exists instead of just adding qtwindowtest.elf to the normal
# shared build/ext2.img: that image travels whole into RAM as a GRUB
# module for every LIVE_ISO boot (tools/mkext2.py's own NUM_GROUPS
# comment) — growing it to fit one more ~13 MB real Qt binary directly
# shrinks the physical RAM left over to actually *execute* one (a real,
# confirmed "out of memory" at the ash prompt, not a hypothetical
# concern). A small, dedicated scratch image sidesteps this tension
# entirely without repeatedly fighting the shared disk's own size for
# every Qt QPA program written during this phase; fold qtwindowtest.elf
# (or whatever real QtWidgets program eventually replaces it) into the
# permanent shared disk only once Phase 6 stabilizes and its real size
# requirements are known (see the Makefile's own QT_STANDALONE_ELFS
# comment).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD="${REPO_ROOT}/build"

command -v i686-elf-grub-mkrescue >/dev/null 2>&1 || {
  echo "i686-elf-grub-mkrescue is required (e.g. 'brew install i686-elf-grub')" >&2
  exit 1
}

make -C "${REPO_ROOT}" "${BUILD}/pureunix.elf" "${BUILD}/user/busybox.elf" "${BUILD}/user/qtwindowtest.elf"

echo "==> Building scratch FAT16 (empty-ish, just needed as GRUB module 'fat.img')"
python3 "${SCRIPT_DIR}/mkfat16.py" "${BUILD}/qpa-scratch-fat.img"

echo "==> Building scratch EXT2 (BusyBox + qtwindowtest.elf only)"
python3 "${SCRIPT_DIR}/mkext2.py" "${BUILD}/qpa-scratch-root.img" \
  "${BUILD}/user/busybox.elf" "${BUILD}/user/qtwindowtest.elf"

echo "==> Assembling build/qpa-scratch.iso"
rm -rf "${BUILD}/qpa-scratch-iso"
mkdir -p "${BUILD}/qpa-scratch-iso/boot/grub"
cp "${BUILD}/pureunix.elf" "${BUILD}/qpa-scratch-iso/boot/pureunix.elf"
cp "${BUILD}/qpa-scratch-fat.img" "${BUILD}/qpa-scratch-iso/boot/fat.img"
cp "${BUILD}/qpa-scratch-root.img" "${BUILD}/qpa-scratch-iso/boot/root.img"
cp "${REPO_ROOT}/boot/grub.cfg" "${BUILD}/qpa-scratch-iso/boot/grub/grub.cfg"
i686-elf-grub-mkrescue -o "${BUILD}/qpa-scratch.iso" "${BUILD}/qpa-scratch-iso"

echo "==> Done: ${BUILD}/qpa-scratch.iso"
echo "==> Test with: python3 tools/vt-inject-test.py --iso build/qpa-scratch.iso <script.txt>"
