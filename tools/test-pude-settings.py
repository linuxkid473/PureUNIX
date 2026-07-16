#!/usr/bin/env python3
"""tools/test-pude-settings.py — end-to-end QEMU test for pude's Settings
app and desktop wallpaper (docs/pude.md's "Settings" section, user/
pude_settings.c/.h, user/pude_wallpaper.c/.h). Same overall technique as
tools/test-pude-dock.py (real injected mouse input via HMP mouse_move/
mouse_button, since QMP input-send-event doesn't move this environment's
real PS/2 mouse) and tools/test-imgview.py (a scratch persistent disk built
from the real Makefile recipe, real PNG fixtures from tools/gen-test-pngs.py,
and a real second/independent QEMU process to prove reboot persistence, not
just a runtime write+re-read).

To avoid the picker's own row-index-in-a-big-root-directory guessing game,
this test pre-seeds two things onto the scratch disk before boot:
  - /etc/pude.conf naming a real wallpaper (/wallpapers/seed.png) --
    proves pude_wallpaper_init() applies a configured wallpaper at
    startup, and gives the Settings app a known "Choose..." start
    directory (dirname of the current wallpaper).
  - /wallpapers/ containing exactly four files, sorted deterministically
    (bad.png, notes.txt, seed.png, tiny2.png) -- so every row in the file
    picker's listing has a known, fixed index for the whole test.

Exercises, in order:
  1. pude starts with the seeded wallpaper already applied (solid color
     fills the desktop) -- proves startup decode from /etc/pude.conf.
  2. Opening Settings from its dock icon (5th pinned app) shows the
     current wallpaper path.
  3. Choosing a non-PNG file (notes.txt) is rejected by the picker without
     closing it (docs/pude.md: "Only allow PNG images").
  4. Choosing a same-extension-but-corrupt file (bad.png) fails to apply
     and falls back to the previous wallpaper, not a crash or a blank
     screen (docs/pude.md: "If the wallpaper cannot be loaded, fall back
     to the existing solid background").
  5. Choosing a second, real, distinctly-colored PNG (tiny2.png) applies
     immediately (a real screendump pixel check, not just "no crash").
  6. A completely independent second QEMU process against the same
     on-disk image proves the newly-chosen wallpaper (not just the
     originally-seeded one) survives a real reboot.

Usage:
    tools/test-pude-settings.py
"""
import importlib
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
    """Identical technique to tools/test-pude-dock.py's own QemuSession
    (duplicated rather than shared, matching that file's own precedent)."""

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
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-serial", f"file:{serial_log}",
            "-qmp", f"unix:{qmp_sock},server,nowait",
            "-no-reboot", "-no-shutdown", "-display", "none",
        ]
        self.proc = subprocess.Popen(args)
        self.sock = self._connect()
        self.buf = [b""]
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
        while b"\n" not in self.buf[0]:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP socket closed unexpectedly")
            self.buf[0] += chunk
        line, rest = self.buf[0].split(b"\n", 1)
        self.buf[0] = rest
        return json.loads(line.decode())

    def _cmd(self, name, **kwargs):
        payload = {"execute": name}
        if kwargs:
            payload["arguments"] = kwargs
        self.sock.sendall((json.dumps(payload) + "\n").encode())
        while True:
            resp = self._recv_line()
            if "return" in resp or "error" in resp:
                return resp

    def send_keys(self, keys):
        self._cmd("send-key", keys=keys)

    def send_key_combo(self, combo):
        self.send_keys([{"type": "qcode", "data": part} for part in combo.split("+")])

    def type_text(self, text, delay=0.02):
        for ch in text:
            self.send_keys(char_to_keys(ch))
            time.sleep(delay)

    def screendump(self, path):
        resp = self._cmd("screendump", filename=path)
        if "error" in resp:
            raise RuntimeError(f"screendump failed: {resp['error']}")
        time.sleep(0.3)

    def mouse_rel(self, dx, dy):
        self._cmd("human-monitor-command", **{"command-line": f"mouse_move {dx} {dy}"})

    def mouse_button(self, down, button_mask=1):
        mask = button_mask if down else 0
        self._cmd("human-monitor-command", **{"command-line": f"mouse_button {mask}"})

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
        raise TimeoutError(f"timed out waiting for {pattern!r}; last output:\n{self.tail()[-3000:]}")

    def kill(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)


# ---- pude's chrome geometry (user/pude.c) -- kept in sync by hand, same
# convention as tools/test-pude-dock.py's own copy. ----------------------
SCREEN_W, SCREEN_H = 1280, 800

DOCK_ICON = 44
DOCK_GAP = 10
DOCK_PAD = 10
DOCK_MARGIN_B = 14
NUM_PINNED = 5  # PUTerm, Calculator, PUFiles, PUText, Settings -- g_apps[]
SETTINGS_DOCK_INDEX = 4

BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2
MARGIN = 24

SETTINGS_CLIENT_W, SETTINGS_CLIENT_H = 420, 260

# user/pude_settings.c's own layout constants.
ST_PAD = 16
ST_BTN_W, ST_BTN_H = 120, 28

# user/pude_filepicker.h's own row height (FONT_CELL_H + 4).
FP_ROW_H = FONT_CELL_H + 4


def dock_rect(screen_w=SCREEN_W, screen_h=SCREEN_H):
    dw = DOCK_PAD * 2 + NUM_PINNED * DOCK_ICON + (NUM_PINNED - 1) * DOCK_GAP
    dh = DOCK_PAD * 2 + DOCK_ICON
    dx = (screen_w - dw) // 2
    dy = screen_h - dh - DOCK_MARGIN_B
    return dx, dy, dw, dh


def dock_icon_center(i, screen_w=SCREEN_W, screen_h=SCREEN_H):
    dx, dy, _, _ = dock_rect(screen_w, screen_h)
    x = dx + DOCK_PAD + i * (DOCK_ICON + DOCK_GAP)
    y = dy + DOCK_PAD
    return x + DOCK_ICON // 2, y + DOCK_ICON // 2


def spawn_position(spawn_index):
    """Mirrors user/pude.c's spawn_window() cascade."""
    cascade = (spawn_index % 8) * 26
    return MARGIN + cascade, MARGIN + cascade


def settings_client_origin(spawn_index=0):
    wx, wy = spawn_position(spawn_index)
    return wx + BORDER, wy + BORDER + TITLEBAR_H


def settings_choose_btn_center(spawn_index=0):
    cx, cy = settings_client_origin(spawn_index)
    header_y = ST_PAD
    path_y = header_y + FONT_CELL_H + 10
    btn_x = ST_PAD
    btn_y = path_y + FONT_CELL_H + 12
    return cx + btn_x + ST_BTN_W // 2, cy + btn_y + ST_BTN_H // 2


def settings_modal_rect(spawn_index=0):
    """Mirrors user/pude_settings.c's settings_modal_rect()."""
    cx, cy = settings_client_origin(spawn_index)
    w = min(380, max(60, SETTINGS_CLIENT_W - 16))
    h = min(300, max(80, SETTINGS_CLIENT_H - 16))
    dx = cx + (SETTINGS_CLIENT_W - w) // 2
    dy = cy + (SETTINGS_CLIENT_H - h) // 2
    return dx, dy, w, h


def fp_row_center(row_index, spawn_index=0):
    """Mirrors user/pude_filepicker.h's pu_filepicker_layout(): list_y is
    right after the fixed-height path row (22px)."""
    dx, dy, dw, dh = settings_modal_rect(spawn_index)
    list_y = dy + 22
    x = dx + 100
    y = list_y + row_index * FP_ROW_H + FP_ROW_H // 2
    return x, y


def fp_confirm_btn_center(spawn_index=0):
    """Mirrors pu_filepicker_layout()'s confirm_btn placement: path_h +
    filename_h + list_h + status_h always sums to (dh - buttons_h)
    regardless of their individual values (list_h is defined as whatever
    remains), so the buttons' row starts at dy + dh - buttons_h."""
    dx, dy, dw, dh = settings_modal_rect(spawn_index)
    bw, bh, gap = 92, 26, 12
    buttons_h = 34
    bx0 = dx + dw - 2 * bw - gap - 10
    button_top_y = dy + dh - buttons_h + (buttons_h - bh) // 2
    confirm_x = bx0 + bw + gap
    return confirm_x + bw // 2, button_top_y + bh // 2


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        dims = f.readline()
        while dims.startswith(b"#"):
            dims = f.readline()
        w, h = (int(x) for x in dims.split())
        f.readline()  # maxval
        data = f.read(w * h * 3)
    return w, h, data


def get_pixel(w, h, data, x, y):
    idx = (y * w + x) * 3
    return tuple(data[idx:idx + 3])


def close_enough(a, b, tol=8):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def make_recipe_lines(target, touch_file):
    """Identical technique to tools/test-imgview.py's own helper."""
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


def build_scratch_disk(tmp, extra_files):
    """Builds a scratch persistent EXT2 image + MBR/GRUB ISO with the given
    {dest_path: host_path} files added, reusing the real build's own ELF/
    kernel/GRUB artifacts (assumes `make iso` has already been run)."""
    for f in ("build/pureunix.elf", "build/grub/boot.img", "build/grub/core.img"):
        if not os.path.exists(os.path.join(REPO_ROOT, f)):
            raise RuntimeError(f"{f} is missing -- run `make iso` first")

    ext2_lines = make_recipe_lines("build/ext2-persistent.img", "tools/mkext2.py")
    ext2_cmd = "\n".join(ext2_lines)
    scratch_ext2 = os.path.join(tmp, "test-ext2.img")
    ext2_cmd = ext2_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    for dest_path, host_path in extra_files.items():
        ext2_cmd += f" --extra-file {host_path}:{dest_path}"
    subprocess.run(ext2_cmd, cwd=REPO_ROOT, shell=True, check=True)

    iso_lines = make_recipe_lines("build/pureunix.iso", "tools/mkdiskimg.py")
    iso_cmd = "\n".join(iso_lines)
    scratch_iso = os.path.join(tmp, "test.iso")
    iso_cmd = iso_cmd.replace("build/pureunix.iso", scratch_iso, 1)
    iso_cmd = iso_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    subprocess.run(iso_cmd, cwd=REPO_ROOT, shell=True, check=True)

    return scratch_iso


SEED_RGB = (0, 180, 220)
TINY2_RGB = (220, 90, 10)

# A point on the plain desktop guaranteed clear of the dock (bottom-
# center), the menu/drawer buttons (top-left), and any window this test
# ever opens (Settings only, cascaded at MARGIN,MARGIN) -- used to sample
# the current wallpaper color.
DESKTOP_SAMPLE = (SCREEN_W - 60, 120)


def main():
    failures = []

    with tempfile.TemporaryDirectory(prefix="pureunix-pude-settings-test-") as tmp:
        wp_dir = os.path.join(tmp, "wallpapers")
        os.makedirs(wp_dir, exist_ok=True)

        seed_png = os.path.join(wp_dir, "seed.png")
        tiny2_png = os.path.join(wp_dir, "tiny2.png")
        bad_png = os.path.join(wp_dir, "bad.png")
        notes_txt = os.path.join(wp_dir, "notes.txt")

        gen_test_pngs.write_png(seed_png, 16, 16, 2, lambda x, y: bytes(SEED_RGB))
        gen_test_pngs.write_png(tiny2_png, 20, 20, 2, lambda x, y: bytes(TINY2_RGB))
        with open(bad_png, "wb") as f:
            f.write(b"this is not really a png\n")
        with open(notes_txt, "w") as f:
            f.write("just some notes, not an image\n")

        conf_path = os.path.join(tmp, "pude.conf")
        with open(conf_path, "w") as f:
            f.write("wallpaper=/wallpapers/seed.png\n")

        print("=== building scratch disk with wallpaper fixtures + seeded /etc/pude.conf ===")
        iso = build_scratch_disk(tmp, {
            "/wallpapers/seed.png": seed_png,
            "/wallpapers/tiny2.png": tiny2_png,
            "/wallpapers/bad.png": bad_png,
            "/wallpapers/notes.txt": notes_txt,
            "/etc/pude.conf": conf_path,
        })

        # /wallpapers/ sorted (pu_fp_compare: ".." first, then case-
        # insensitive name) -- deterministic since it's a fresh directory
        # with only these four files.
        ROW_DOTDOT, ROW_BAD, ROW_NOTES, ROW_SEED, ROW_TINY2 = 0, 1, 2, 3, 4

        serial1 = os.path.join(tmp, "serial1.log")
        qmp1 = os.path.join(tmp, "qmp1.sock")
        print("=== boot 1: startup wallpaper, Settings UI, apply/reject/fallback ===")
        qemu = QemuSession(iso, serial1, qmp1)
        shots = []

        def shot(name):
            path = os.path.join(tmp, name)
            qemu.screendump(path)
            shots.append(path)
            print(f"screenshot: {path}")
            return path

        try:
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            print("--- launching pude ---")
            qemu.type_text("pude\n")
            time.sleep(2.0)
            p = shot("s01-desktop-startup-wallpaper.ppm")
            w, h, data = read_ppm(p)
            actual = get_pixel(w, h, data, *DESKTOP_SAMPLE)
            if not close_enough(actual, SEED_RGB):
                failures.append(f"startup: desktop pixel expected {SEED_RGB}, got {actual}")
            else:
                print(f"    startup wallpaper color OK: {actual}")

            print("--- opening Settings from the dock ---")
            qemu.click_at(*dock_icon_center(SETTINGS_DOCK_INDEX))
            time.sleep(0.8)
            shot("s02-settings-open.ppm")

            print("--- opening the file picker (Choose...) ---")
            qemu.click_at(*settings_choose_btn_center())
            time.sleep(0.4)
            shot("s03-picker-in-wallpapers-dir.ppm")

            print("--- selecting notes.txt (non-PNG) and confirming: must be rejected ---")
            qemu.click_at(*fp_row_center(ROW_NOTES))
            time.sleep(0.2)
            qemu.click_at(*fp_confirm_btn_center())
            time.sleep(0.3)
            shot("s04-notes-txt-rejected-picker-still-open.ppm")

            print("--- selecting bad.png (invalid PNG) and confirming: must fail gracefully ---")
            qemu.click_at(*fp_row_center(ROW_BAD))
            time.sleep(0.2)
            qemu.click_at(*fp_confirm_btn_center())
            time.sleep(0.4)
            p = shot("s05-bad-png-failed-fallback.ppm")
            w, h, data = read_ppm(p)
            actual = get_pixel(w, h, data, *DESKTOP_SAMPLE)
            if not close_enough(actual, SEED_RGB):
                failures.append(f"bad.png fallback: desktop pixel expected {SEED_RGB} (unchanged), got {actual}")
            else:
                print(f"    fallback preserved previous wallpaper OK: {actual}")

            print("--- reopening picker and selecting tiny2.png (valid, distinct color) ---")
            qemu.click_at(*settings_choose_btn_center())
            time.sleep(0.4)
            qemu.click_at(*fp_row_center(ROW_TINY2))
            time.sleep(0.2)
            qemu.click_at(*fp_confirm_btn_center())
            time.sleep(0.4)
            p = shot("s06-tiny2-applied-immediately.ppm")
            w, h, data = read_ppm(p)
            actual = get_pixel(w, h, data, *DESKTOP_SAMPLE)
            if not close_enough(actual, TINY2_RGB):
                failures.append(f"tiny2.png apply: desktop pixel expected {TINY2_RGB}, got {actual}")
            else:
                print(f"    new wallpaper applied immediately OK: {actual}")

            print("--- emergency whole-desktop quit (Ctrl+F12) back to outer ash ---")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            qemu.type_text("echo settings_test_wm_exited_ok\n")
            qemu.wait_for(r"settings_test_wm_exited_ok", 20)
        finally:
            qemu.kill()  # hard kill -- the reboot test below must not rely
                         # on a clean shutdown having flushed anything extra
                         # (same rationale as tools/test-imgview.py's own
                         # reboot test / tools/test-persistent-boot.py).

        time.sleep(1)

        print("=== boot 2: same on-disk image, fresh QEMU process (reboot) ===")
        serial2 = os.path.join(tmp, "serial2.log")
        qmp2 = os.path.join(tmp, "qmp2.sock")
        qemu2 = QemuSession(iso, serial2, qmp2)
        try:
            qemu2.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)
            qemu2.type_text("pude\n")
            time.sleep(2.0)
            p = os.path.join(tmp, "s07-reboot-wallpaper-persisted.ppm")
            qemu2.screendump(p)
            time.sleep(0.3)
            print(f"screenshot: {p}")
            w, h, data = read_ppm(p)
            actual = get_pixel(w, h, data, *DESKTOP_SAMPLE)
            if not close_enough(actual, TINY2_RGB):
                failures.append(f"reboot: desktop pixel expected {TINY2_RGB} (persisted), got {actual}")
            else:
                print(f"    wallpaper persisted across reboot OK: {actual}")
            qemu2.send_key_combo("ctrl+f12")
            time.sleep(0.5)
        finally:
            qemu2.kill()

    print()
    if failures:
        print(f"=== FAILED ({len(failures)} issue(s)) ===")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("=== PASS: pude Settings/wallpaper test complete ===")


if __name__ == "__main__":
    main()
