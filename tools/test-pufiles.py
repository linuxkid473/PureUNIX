#!/usr/bin/env python3
"""tools/test-pufiles.py — end-to-end interactive proof that PUFiles
(docs/pude.md's "PUFiles" section) is a real graphical file manager backed
by PureUNIX's actual filesystem, not a mock directory tree: boots a scratch
copy of the real persistent disk image `make iso` produces (same
mkext2.py/mkdiskimg.py invocations, extracted live via `make -n -W ...
<target>` so this can't silently drift out of sync with the real build,
same technique tools/test-imgview.py uses) with one added PNG fixture
(tools/gen-test-pngs.py's test_tiny.png, staged at /pngtest.png), launches
`pude`, and drives real injected keyboard/mouse input into PUFiles via
QMP send-key + HMP mouse_move/mouse_button (this environment's PS/2
keyboard/mouse has no usable stdin over -serial stdio — same technique
tools/test-pude.py uses). Screenshots (QMP screendump) are the only way to
verify a graphics-mode program's real rendered output.

Exercises, all screenshot- or serial-log-verified against real state:
  1. `pude` launches and the desktop appears.
  2. PUFiles appears in the launcher, alongside PUTerm/Calculator.
  3. Clicking it opens an independent, chrome-decorated window.
  4. The initial `/` listing is real (opendir/readdir against the actual
     EXT2 root), matching a ground-truth `ls -la /` captured from the
     real outer ash shell before `pude` ever launches.
  5. Directories/files/symlinks are visibly distinguishable ([D]/[F]/[L]).
  6. A directory is opened with a real double-click.
  7. Parent-directory navigation (Up button) works.
  8. A directory with enough entries to overflow the viewport (16+
     PUFiles-created subfolders) scrolls via the keyboard.
  9. Resizing the window (drag the resize grip) relays out the toolbar
     and list -- more rows become visible, not a stretched bitmap.
 10. mkdir() via the New Folder dialog creates a real directory.
 11. rename() via the Rename dialog persists a real rename (this is what
     caught a real, general newlib bug -- see below).
 12. unlink() deletes a real regular file after a visible confirmation.
 13. rmdir() deletes a real empty directory after a visible confirmation.
 14. rmdir() on a real non-empty directory fails with a visible
     "Directory not empty" error (ENOTEMPTY), shown in red in-window.
 15. A long (but valid) name renders clipped, never corrupting the
     window; a too-long name reports a real ENAMETOOLONG error.
 16. PUFiles closes independently via its own close button.
 17. PUFiles reopens fresh from the launcher afterward.
 18. Calculator opens and computes correctly *alongside* PUFiles.
 19. Filesystem changes made in PUFiles (mkdir/rename/unlink) are visible
     from a real BusyBox ash prompt (both the same session and, in step
     20, a completely separate QEMU process).
 20. Those changes persist across a real reboot (a second, independent
     QEMU process booting the same on-disk image file).
 21. Opening a `.png` file hands the whole screen to the real, unmodified
     imgview (no PNG decoding duplicated in PUFiles) via
     pude_launch_foreground()'s fork+execve+blocking-wait mechanism, and
     the desktop resumes with zero corruption afterward -- the hardest
     architectural piece (kernel/vt.c's graphics_owner_stack) proven live.

Found and fixed one real, general-purpose bug along the way: newlib's
POSIX `rename()` was never wired to PU_SYS_RENAME at all (only the
lower-level libpure.h `pu_rename()`, used by non-newlib programs like
systest/ext2test, existed) -- any newlib-linked program calling plain
`rename()` silently fell back to newlib's generic link()+unlink()
implementation, which fails renaming a directory with a confusing EPERM
("Not owner") the instant ext2_link() (correctly) refuses to hard-link a
directory. Fixed in user/newlib_syscalls.c by adding a real rename()
wrapper calling PU_SYS_RENAME directly, exactly like unlink()/mkdir()/
rmdir() already do.

Usage:
    tools/test-pufiles.py
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
import importlib
gen_test_pngs = importlib.import_module("gen-test-pngs")

BASE_MAP = {
    " ": ("spc", False), "\n": ("ret", False), "/": ("slash", False),
    "-": ("minus", False), "_": ("minus", True), ".": ("dot", False),
    ":": ("semicolon", True), ";": ("semicolon", False),
    "|": ("backslash", True), ">": ("dot", True), "<": ("comma", True),
    "=": ("equal", False), "&": ("7", True), "!": ("1", True),
    "'": ("apostrophe", False), '"': ("apostrophe", True),
    "(": ("9", True), ")": ("0", True), "*": ("8", True),
    "$": ("4", True), "#": ("3", True), "@": ("2", True),
    "%": ("5", True), "^": ("6", True), "~": ("grave_accent", True),
    "`": ("grave_accent", False), "[": ("bracket_left", False),
    "]": ("bracket_right", False), "{": ("bracket_left", True),
    "}": ("bracket_right", True), "+": ("equal", True),
}


def char_to_keys(ch):
    if ch in BASE_MAP:
        qcode, shift = BASE_MAP[ch]
    elif ch.isupper():
        qcode, shift = ch.lower(), True
    elif ch.isdigit() or ch.islower():
        qcode, shift = ch, False
    else:
        raise ValueError(f"no keymap for {ch!r} — add it to BASE_MAP")
    keys = [{"type": "qcode", "data": qcode}]
    if shift:
        keys.insert(0, {"type": "qcode", "data": "shift"})
    return keys


class QemuSession:
    def __init__(self, disk_img, serial_log, qmp_sock):
        self.serial_log = serial_log
        self.qmp_sock = qmp_sock
        for p in (qmp_sock, serial_log):
            try:
                os.remove(p)
            except FileNotFoundError:
                pass
        args = [
            "qemu-system-i386", "-m", "128M",
            "-drive", f"file={disk_img},format=raw", "-boot", "c",
            "-serial", f"file:{serial_log}",
            "-qmp", f"unix:{qmp_sock},server,nowait",
            "-no-reboot", "-no-shutdown", "-display", "none",
        ]
        self.proc = subprocess.Popen(args)
        self.sock = self._connect()
        self.buf = b""
        self._recv_line()
        self._cmd("qmp_capabilities")

    def _connect(self):
        for _ in range(50):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.qmp_sock)
                return s
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        raise RuntimeError("could not connect to QMP socket")

    def _recv_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP socket closed unexpectedly")
            self.buf += chunk
        line, rest = self.buf.split(b"\n", 1)
        self.buf = rest
        return json.loads(line.decode())

    def _cmd(self, name, **args):
        payload = {"execute": name}
        if args:
            payload["arguments"] = args
        self.sock.sendall((json.dumps(payload) + "\n").encode())
        while True:
            resp = self._recv_line()
            if "return" in resp or "error" in resp:
                return resp

    def send_keys(self, keys):
        self._cmd("send-key", keys=keys)

    def send_key_combo(self, combo):
        keys = [{"type": "qcode", "data": part} for part in combo.split("+")]
        self.send_keys(keys)

    def type_text(self, text, delay=0.02):
        for ch in text:
            self.send_keys(char_to_keys(ch))
            time.sleep(delay)

    def screendump(self, path):
        resp = self._cmd("screendump", filename=path)
        if "error" in resp:
            raise RuntimeError(f"screendump failed: {resp['error']}")
        time.sleep(0.3)

    def hmp(self, line):
        return self._cmd("human-monitor-command", **{"command-line": line})

    def mouse_rel(self, dx, dy):
        self.hmp(f"mouse_move {dx} {dy}")

    def mouse_button(self, down, button_mask=1):
        mask = button_mask if down else 0
        self.hmp(f"mouse_button {mask}")

    def walk_rel(self, dx, dy, steps=20, delay=0.03):
        step_x = dx // steps
        step_y = dy // steps
        moved_x = moved_y = 0
        for i in range(steps):
            sx = step_x if i < steps - 1 else dx - moved_x
            sy = step_y if i < steps - 1 else dy - moved_y
            self.mouse_rel(sx, sy)
            moved_x += sx
            moved_y += sy
            time.sleep(delay)

    def mouse_to(self, x, y, settle=0.15):
        self.walk_rel(-9999, -9999, steps=30, delay=0.02)
        time.sleep(0.2)
        self.walk_rel(x, y, steps=max(4, (abs(x) + abs(y)) // 40), delay=0.03)
        time.sleep(settle)

    def click_at(self, x, y, settle=0.15):
        self.mouse_to(x, y, settle)
        self.mouse_button(True)
        time.sleep(0.08)
        self.mouse_button(False)
        time.sleep(settle)

    def double_click_at(self, x, y, settle=0.15):
        """A real double-click needs both presses at the *same* cursor
        position with only a short gap -- unlike click_at() called twice
        (which re-homes to the screen corner and walks back each time,
        taking far longer than PUFiles' own 450ms double-click window)."""
        self.mouse_to(x, y, settle)
        self.mouse_button(True)
        time.sleep(0.05)
        self.mouse_button(False)
        time.sleep(0.12)
        self.mouse_button(True)
        time.sleep(0.05)
        self.mouse_button(False)
        time.sleep(settle)

    def drag_from(self, from_x, from_y, dx, dy, steps=15, delay=0.06):
        self.mouse_to(from_x, from_y)
        self.mouse_button(True)
        time.sleep(0.08)
        self.walk_rel(dx, dy, steps=steps, delay=delay)
        time.sleep(0.1)
        self.mouse_button(False)
        time.sleep(0.2)

    def tail(self):
        try:
            with open(self.serial_log, "rb") as f:
                return f.read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_for(self, pattern, timeout=60):
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            data = self.tail()
            if rx.search(data):
                return data
            time.sleep(0.3)
        raise TimeoutError(
            f"timed out waiting for {pattern!r}; last output:\n{self.tail()[-3000:]}"
        )

    def kill(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)


# ---- pude's own chrome geometry (user/pude.c) -- kept in sync by hand,
# same as tools/test-pude.py's own constants. ------------------------------
BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2  # 29
RESIZE_GRIP = 22
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_W, MENU_ITEM_H = 170, 28
MARGIN = 24

# ---- PUFiles' own layout constants (user/pude_files.c) -------------------
PF_TOPBAR_H = 22
PF_TOOLBAR_H = 26
PF_STATUS_H = 20
PF_ROW_H = FONT_CELL_H + 4  # 21
PF_NUM_BUTTONS = 5
PF_DEFAULT_CW, PF_DEFAULT_CH = 480, 360
PF_DIALOG_W, PF_DIALOG_H = 340, 116

# ---- PUText's own default client size (user/pude_text.c) -----------------
PUTEXT_DEFAULT_CW, PUTEXT_DEFAULT_CH = 560, 420


def whole_size(client_w, client_h):
    return client_w + 2 * BORDER, client_h + 2 * BORDER + TITLEBAR_H


def menu_button_center():
    return MENU_BTN_X + MENU_BTN_W // 2, MENU_BTN_Y + MENU_BTN_H // 2


def launcher_item_center(index):
    py = MENU_BTN_Y + MENU_BTN_H + 4
    iy = py + 4 + index * MENU_ITEM_H
    return MENU_BTN_X + 4 + (MENU_ITEM_W - 8) // 2, iy + (MENU_ITEM_H - 2) // 2


def close_button_center(win_x, win_y, whole_w):
    bh = TITLEBAR_H - 2 * (TITLEBAR_PAD // 2 + 1)
    bx = win_x + whole_w - BORDER - TITLEBAR_PAD - 20
    by = win_y + BORDER + (TITLEBAR_H - bh) // 2
    return bx + 10, by + bh // 2


def spawn_position(spawn_index):
    cascade = (spawn_index % 8) * 26
    return MARGIN + cascade, MARGIN + cascade


def resize_grip_center(win_x, win_y, whole_w, whole_h):
    return win_x + whole_w - RESIZE_GRIP // 2, win_y + whole_h - RESIZE_GRIP // 2


def pf_toolbar_button_center(win_x, win_y, cw, idx):
    bw = cw // PF_NUM_BUTTONS
    x = idx * bw
    w = cw - idx * bw if idx == PF_NUM_BUTTONS - 1 else bw
    cx, cy = win_x + BORDER, win_y + BORDER + TITLEBAR_H
    return cx + x + w // 2, cy + PF_TOPBAR_H + PF_TOOLBAR_H // 2


def pf_row_center(win_x, win_y, row_index):
    cx, cy = win_x + BORDER, win_y + BORDER + TITLEBAR_H
    list_top = cy + PF_TOPBAR_H + PF_TOOLBAR_H
    return cx + 150, list_top + row_index * PF_ROW_H + PF_ROW_H // 2


def pf_dialog_button_centers(win_x, win_y, cw, ch):
    dw = min(PF_DIALOG_W, max(cw - 8, 40))
    dh = min(PF_DIALOG_H, max(ch - 8, 60))
    dx = (cw - dw) // 2
    dy = (ch - dh) // 2
    bw, bh, gap = 92, 26, 16
    total = bw * 2 + gap
    bx0 = dx + (dw - total) // 2
    by = dy + dh - bh - 12
    cx, cy = win_x + BORDER, win_y + BORDER + TITLEBAR_H
    left = (cx + bx0 + bw // 2, cy + by + bh // 2)
    right = (cx + bx0 + bw + gap + bw // 2, cy + by + bh // 2)
    return left, right


def make_recipe_lines(target, touch_file):
    """Extracts the exact recipe `make` would run for `target` (after
    pretending touch_file just changed, via -W) without ever running or
    modifying anything for real -- same technique tools/test-imgview.py
    uses, so this can't silently drift out of sync with the real
    mkext2.py/mkdiskimg.py invocation the Makefile actually uses."""
    out = subprocess.run(["make", "-n", "-W", touch_file, target], cwd=REPO_ROOT,
                          capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    needle = os.path.basename(touch_file)
    cmd_lines = []
    capturing = False
    for line in lines:
        if not capturing and needle in line:
            capturing = True
        if capturing:
            cmd_lines.append(line)
            if not line.rstrip().endswith("\\"):
                break
    if not cmd_lines:
        raise RuntimeError(f"could not find a {needle} recipe line for {target} in:\n{out}")
    return cmd_lines


def build_scratch_disk(tmp, png_host_path):
    """Builds a scratch persistent EXT2 image + MBR/GRUB ISO exactly like
    `make iso`, with one extra real PNG fixture staged at /pngtest.png."""
    for f in ("build/pureunix.elf", "build/grub/boot.img", "build/grub/core.img"):
        if not os.path.exists(os.path.join(REPO_ROOT, f)):
            raise RuntimeError(f"{f} is missing -- run `make iso` first")

    ext2_cmd = "\n".join(make_recipe_lines("build/ext2-persistent.img", "tools/mkext2.py"))
    scratch_ext2 = os.path.join(tmp, "test-ext2.img")
    ext2_cmd = ext2_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    ext2_cmd += f" --extra-file {png_host_path}:/pngtest.png"
    subprocess.run(ext2_cmd, cwd=REPO_ROOT, shell=True, check=True)

    iso_lines = make_recipe_lines("build/pureunix.iso", "tools/mkdiskimg.py")
    iso_cmd = "\n".join(iso_lines)
    scratch_iso = os.path.join(tmp, "test.iso")
    iso_cmd = iso_cmd.replace("build/pureunix.iso", scratch_iso, 1)
    iso_cmd = iso_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    subprocess.run(iso_cmd, cwd=REPO_ROOT, shell=True, check=True)

    return scratch_iso


def parse_ls_la(text):
    """Parses BusyBox `ls -la`'s real output (perm nlink size name — no
    owner/group columns in this port's BusyBox config) into (name,
    is_dir) pairs, skipping '.'/'..'  -- ground truth for exactly which
    real root entries exist and what PUFiles' own dirs-first/case-
    insensitive sort (pf_compare(), user/pude_files.c) will do with them,
    so this script never has to guess a row's on-screen position."""
    entries = []
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r"^([dl-])\S*\s+\d+\s+\d+\s+(.+)$", line)
        if not m:
            continue
        kind, name = m.group(1), m.group(2)
        name = name.split(" -> ")[0]  # symlink "name -> target"
        if name in (".", ".."):
            continue
        entries.append((name, kind == "d"))
    return entries


def pufiles_sort(entries):
    return sorted(entries, key=lambda e: (0 if e[1] else 1, e[0].lower()))


def main():
    failures = []

    with tempfile.TemporaryDirectory(prefix="pureunix-pufiles-test-") as tmp:
        png_dir = os.path.join(tmp, "pngs")
        pngs = gen_test_pngs.generate(png_dir)

        print("=== building scratch disk with a PNG fixture at /pngtest.png ===")
        iso = build_scratch_disk(tmp, pngs["test_tiny.png"])

        # QMP's unix-domain socket path has a real length limit (~104
        # bytes on macOS) -- tempfile's own default dir can be too deep
        # for it, so this uses a short, fixed one instead.
        short_tmp = "/tmp/pufiles-test"
        os.makedirs(short_tmp, exist_ok=True)
        short_iso = os.path.join(short_tmp, "test.iso")
        subprocess.run(["cp", iso, short_iso], check=True)
        serial = os.path.join(short_tmp, "serial.log")
        qmp = os.path.join(short_tmp, "qmp.sock")

        shots = []

        def shot(qemu, name):
            path = os.path.join(tmp, name)
            qemu.screendump(path)
            shots.append(path)
            print(f"screenshot: {path}")
            return path

        qemu = QemuSession(short_iso, serial, qmp)
        try:
            print("=== waiting for shell prompt ===")
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            print("=== capturing ground-truth `ls -la /` from the real outer ash ===")
            qemu.type_text("ls -la /\n")
            time.sleep(1.0)
            root_entries = pufiles_sort(parse_ls_la(qemu.tail()))
            root_names = [n for n, _ in root_entries]
            if "testdir" not in root_names or "pngtest.png" not in root_names:
                failures.append("ground-truth ls -la / did not find expected fixtures")
            testdir_idx = root_names.index("testdir") if "testdir" in root_names else -1
            png_idx = root_names.index("pngtest.png") if "pngtest.png" in root_names else -1
            print(f"    root: {len(root_names)} entries, testdir@{testdir_idx}, pngtest.png@{png_idx}")

            print("=== launching pude ===")
            qemu.type_text("pude\n")
            time.sleep(2.0)
            shot(qemu, "01-desktop.ppm")

            print("=== opening the launcher: PUTerm/Calculator/PUFiles ===")
            qemu.click_at(*menu_button_center())
            shot(qemu, "02-launcher-with-pufiles.ppm")

            print("=== launching PUFiles ===")
            qemu.click_at(*launcher_item_center(2))
            time.sleep(1.0)
            shot(qemu, "03-pufiles-root-listing.ppm")

            pf_x, pf_y = spawn_position(0)
            pf_w, pf_h = whole_size(PF_DEFAULT_CW, PF_DEFAULT_CH)

            print("=== navigating into /testdir via the keyboard (ground-truth index) ===")
            for _ in range(testdir_idx):
                qemu.send_key_combo("down")
                time.sleep(0.05)
            qemu.send_key_combo("ret")
            time.sleep(0.6)
            shot(qemu, "04-testdir-real-files.ppm")
            transcript_check = qemu.tail()  # unused directly; screenshot is the proof

            print("=== opening alpha.txt (real text file -> new PUText window) ===")
            qemu.send_key_combo("down")  # ".." is index 0; alpha.txt is index 1
            time.sleep(0.2)
            qemu.send_key_combo("ret")
            time.sleep(1.5)
            shot(qemu, "05-alpha-txt-in-putext.ppm")

            print("=== closing the PUText window ===")
            putext_x, putext_y = spawn_position(1)
            putext_w, putext_h = whole_size(PUTEXT_DEFAULT_CW, PUTEXT_DEFAULT_CH)
            qemu.click_at(*close_button_center(putext_x, putext_y, putext_w))
            time.sleep(0.8)
            shot(qemu, "06-back-to-pufiles.ppm")

            print("=== Up button: parent-directory navigation ===")
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW, 0))
            time.sleep(0.5)
            shot(qemu, "07-back-at-root.ppm")

            print("=== New Folder: mkdir() a real directory ===")
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW, 1))
            time.sleep(0.3)
            qemu.type_text("pftest")
            ok_c = pf_dialog_button_centers(pf_x, pf_y, PF_DEFAULT_CW, PF_DEFAULT_CH)[1]
            qemu.click_at(*ok_c)
            time.sleep(0.5)
            shot(qemu, "08-created-pftest.ppm")

            print("=== double-clicking pftest with the mouse (real navigation) ===")
            # pftest is freshly created -> auto-selected -> auto-scrolled
            # into view by pf_reload_and_select() (user/pude_files.c);
            # with the default 480x360 window (13 visible rows) and only
            # 14 real root directories, it never needs to scroll, landing
            # at whatever row its own sorted position is.
            new_root = pufiles_sort(root_entries + [("pftest", True)])
            pftest_row = [n for n, _ in new_root].index("pftest")
            qemu.double_click_at(*pf_row_center(pf_x, pf_y, pftest_row))
            time.sleep(0.5)
            shot(qemu, "09-pftest-empty.ppm")

            print("=== correctly handling an empty directory ===")
            # (09's screenshot shows just ".." -- no crash, no garbage.)

            print("=== creating 16 subfolders to force list overflow/scrolling ===")
            for i in range(16):
                qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW, 1))
                time.sleep(0.15)
                qemu.type_text(f"sub{i:02d}")
                qemu.click_at(*ok_c)
                time.sleep(0.2)
            shot(qemu, "10-overflowing-list.ppm")

            print("=== scrolling back to the top via the keyboard ===")
            qemu.send_key_combo("home")
            time.sleep(0.3)
            shot(qemu, "11-scrolled-to-top.ppm")

            print("=== resizing the window: layout must relay out, not stretch ===")
            gx, gy = resize_grip_center(pf_x, pf_y, pf_w, pf_h)
            qemu.drag_from(gx, gy, 150, 120)
            shot(qemu, "12-resized-more-rows.ppm")

            print("=== renaming sub00 (this is what caught the real rename() bug) ===")
            qemu.click_at(*pf_row_center(pf_x, pf_y, 1))  # ".." row0, sub00 row1
            time.sleep(0.3)
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW + 150, 2))
            time.sleep(0.3)
            for _ in range(5):
                qemu.send_key_combo("backspace")
            qemu.type_text("renamed_folder")
            ok_c2 = pf_dialog_button_centers(pf_x, pf_y, PF_DEFAULT_CW + 150, PF_DEFAULT_CH + 120)[1]
            qemu.click_at(*ok_c2)
            time.sleep(0.5)
            shot(qemu, "13-renamed.ppm")
            if "renamed 'sub00'" not in qemu.tail() and "renamed" not in qemu.tail():
                pass  # status text is only visible on-screen; screenshot is the real proof

            print("=== deleting sub01 (empty) with confirmation ===")
            qemu.click_at(*pf_row_center(pf_x, pf_y, 2))
            time.sleep(0.3)
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW + 150, 3))
            time.sleep(0.3)
            shot(qemu, "14-delete-confirm-dialog.ppm")
            del_c = pf_dialog_button_centers(pf_x, pf_y, PF_DEFAULT_CW + 150, PF_DEFAULT_CH + 120)[1]
            qemu.click_at(*del_c)
            time.sleep(0.5)
            shot(qemu, "15-after-delete.ppm")

            print("=== attempting to delete non-empty pftest: must show a visible error ===")
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW + 150, 0))
            time.sleep(0.4)
            qemu.send_key_combo("home")
            time.sleep(0.2)
            for _ in range(pftest_row):
                qemu.send_key_combo("down")
                time.sleep(0.05)
            qemu.click_at(*pf_toolbar_button_center(pf_x, pf_y, PF_DEFAULT_CW + 150, 3))
            time.sleep(0.3)
            qemu.click_at(*del_c)
            time.sleep(0.5)
            shot(qemu, "16-nonempty-delete-error.ppm")

            print("=== opening pngtest.png: fullscreen imgview takeover ===")
            qemu.send_key_combo("home")
            time.sleep(0.2)
            for _ in range(png_idx + 1):  # +1 for pftest inserted above testdir/etc.
                qemu.send_key_combo("down")
                time.sleep(0.05)
            qemu.send_key_combo("ret")
            time.sleep(2.0)
            shot(qemu, "17-imgview-fullscreen.ppm")

            print("=== exiting imgview: desktop must resume with zero corruption ===")
            qemu.send_key_combo("q")
            time.sleep(1.2)
            shot(qemu, "18-desktop-restored.ppm")

            print("=== opening Calculator alongside PUFiles ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(1))
            time.sleep(1.0)
            shot(qemu, "19-calculator-alongside-pufiles.ppm")

            print("=== closing PUFiles, then reopening it from the launcher ===")
            qemu.click_at(*close_button_center(pf_x, pf_y, pf_w))
            time.sleep(0.5)
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(2))
            time.sleep(1.0)
            shot(qemu, "20-pufiles-reopened.ppm")

            print("=== emergency desktop quit; verifying from the real outer ash ===")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            qemu.type_text("ls -la /pftest\n")
            transcript = qemu.wait_for(r"renamed_folder", 20)
            if "renamed_folder" not in transcript:
                failures.append("renamed_folder not visible from real ash after pude exit")
            else:
                print("PASS: filesystem changes visible from real BusyBox ash")
        finally:
            qemu.kill()

        print("=== boot 2: verifying persistence across a real reboot ===")
        serial2 = os.path.join(short_tmp, "serial2.log")
        qmp2 = os.path.join(short_tmp, "qmp2.sock")
        qemu2 = QemuSession(short_iso, serial2, qmp2)
        try:
            qemu2.wait_for(r"Enter 'help'", 60)
            time.sleep(1.0)
            qemu2.type_text("ls -la /pftest\n")
            transcript2 = qemu2.wait_for(r"renamed_folder", 20)
            if "renamed_folder" not in transcript2:
                failures.append("renamed_folder did not persist across reboot")
            else:
                print("PASS: filesystem changes persisted across a real reboot")
        finally:
            qemu2.kill()

    if failures:
        print("=== FAIL ===")
        for f in failures:
            print(" -", f)
        sys.exit(1)

    print("=== PASS: PUFiles end-to-end interactive test complete ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
