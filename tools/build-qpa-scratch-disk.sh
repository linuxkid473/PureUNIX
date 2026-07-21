#!/usr/bin/env bash
# tools/build-qpa-scratch-disk.sh — builds a small, standalone ISO
# (build/qpa-scratch.iso) carrying BusyBox + user/qtwindowtest.elf +
# user/qtwidgetstest.elf + user/pude.elf on its EXT2 root, for
# interactively verifying the "pureunix" QPA plugin (docs/qt-port.md
# Phase 6, user/qpa_pureunix/) in QEMU. pude.elf is included so the real
# end-to-end path (PUDE's "Qt Application"/"Qt Widgets Test" menu items
# fork()+exec()'ing the two real Qt binaries, per user/pude_qtclient.c)
# can be exercised, not just the standalone QPA client.
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

make -C "${REPO_ROOT}" "${BUILD}/pureunix.elf" "${BUILD}/user/busybox.elf" "${BUILD}/user/qtwindowtest.elf" "${BUILD}/user/qtwidgetstest.elf" "${BUILD}/user/pude.elf"

echo "==> Building scratch EXT2 (BusyBox + qtwindowtest.elf + qtwidgetstest.elf + pude.elf)"
# DejaVuSans.ttf at /lib/fonts/ + QT_QPA_FONTDIR (set in user/pude_qtclient.c
# before execve()) is what fixes every Qt app's "tofu box" glyph rendering
# (docs/qt-port.md's Known gap #3) -- included here too so this is the one
# place that actually exercises real Qt widget/text rendering in QEMU.
python3 "${SCRIPT_DIR}/mkext2.py" "${BUILD}/qpa-scratch-root.img" \
  "${BUILD}/user/busybox.elf" "${BUILD}/user/qtwindowtest.elf" "${BUILD}/user/qtwidgetstest.elf" "${BUILD}/user/pude.elf" \
  --extra-file "${REPO_ROOT}/third_party/dejavu-fonts/DejaVuSans.ttf:/lib/fonts/DejaVuSans.ttf"

echo "==> Assembling build/qpa-scratch.iso"
# Deliberately NO fat.img GRUB module here (unlike the shared LIVE_ISO's
# boot/grub.cfg) -- kernel/main.c's fat16_vfs_ops() mount is optional
# ("No FAT16 disk found; /fat will be unavailable." is a normal, non-fatal
# log line, not a boot failure), and skipping it frees a real 32 MiB of
# physical RAM this small machine badly needs: every GRUB module is loaded
# whole into RAM before the kernel starts (see tools/mkext2.py's own
# NUM_GROUPS comment / this repo's "GRUB module RAM ceiling" gotcha), and
# qtwindowtest.elf's real exec() was observed failing with ENOMEM at the
# default 128 MiB machine size specifically because fat.img's now-unused
# 32 MiB was still eating into the same physical RAM pool a real Qt
# binary needs to load into.
rm -rf "${BUILD}/qpa-scratch-iso"
mkdir -p "${BUILD}/qpa-scratch-iso/boot/grub"
cp "${BUILD}/pureunix.elf" "${BUILD}/qpa-scratch-iso/boot/pureunix.elf"
cp "${BUILD}/qpa-scratch-root.img" "${BUILD}/qpa-scratch-iso/boot/root.img"
cat > "${BUILD}/qpa-scratch-iso/boot/grub/grub.cfg" <<'GRUBCFG'
set timeout=1
set default=0

serial --unit=0 --speed=115200
terminal_output console serial

menuentry "PureUnix" {
    multiboot2 /boot/pureunix.elf
    module2 /boot/root.img root.img
    boot
}
GRUBCFG
i686-elf-grub-mkrescue -o "${BUILD}/qpa-scratch.iso" "${BUILD}/qpa-scratch-iso"

echo "==> Done: ${BUILD}/qpa-scratch.iso"
echo "==> Test with: python3 tools/vt-inject-test.py --iso build/qpa-scratch.iso <script.txt>"
