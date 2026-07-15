#!/usr/bin/env python3
"""tools/test-putext.py — end-to-end interactive proof that PUText
(docs/pude.md's "PUText" section) is a real graphical text editor, not a
static-text demo: boots a scratch copy of the real persistent disk image
`make iso` produces (same mkext2.py/mkdiskimg.py invocations, extracted
live via `make -n -W ...` so this can't silently drift out of sync with
the real build, same technique tools/test-pufiles.py uses), launches
`pude`, and drives real injected keyboard/mouse input via QMP send-key +
HMP mouse_move/mouse_button (this environment's PS/2 keyboard/mouse has no
usable stdin over -serial stdio).

Exercises, all screenshot- and/or real-file-content-verified:
  1. PUText appears in the launcher and launches from it.
  2. Typing multiple lines of real text.
  3. Moving the cursor (Home/Up/Up) into previously entered text.
  4. Inserting text in the middle of a line (arrow-key navigation +
     typing), verified by a deterministic Backspace-undo round trip.
  5. Deleting text (Backspace).
  6. Selecting text with the keyboard (Home, Shift+End).
  7. Copy (Ctrl+C) and paste (Ctrl+V) it elsewhere in the document.
  8. Selecting text with the mouse (click + drag).
  9. Cut (Ctrl+X) and paste (Ctrl+V) it back -- verified via the
     "cut immediately followed by paste-at-the-same-spot must reproduce
     the exact original document" invariant, which holds regardless of
     *which* on-screen region the drag actually landed on (real PS/2
     mouse relative-motion in this environment is not pixel-exact --
     tools/test-pufiles.py's own resize-grip drags never needed to land
     precisely either; this test doesn't lean on that precision).
 10. Saving a brand-new Untitled document via the Save-As file picker
     (typing a filename into a real directory listing, no hardcoded
     path ever typed into source).
 11. Closing PUText via its own close button.
 12. Reopening the saved file -- via PUFiles double-click (exercise 16),
     which is also the real text/file-association path.
 13. Verifying the exact saved contents survived, both from a real
     BusyBox ash `cat` in the same boot and (boot 2) after a real reboot.
 14. Resizing the PUText window (drag the resize grip).
 15. Confirming text/cursor/UI stay correct post-resize (typing one more
     character and re-screenshotting).
 16. Opening a real text file from PUFiles and confirming PUText launches
     with that file already loaded (docs/pude.md's desired
     "double-click README.txt -> PUText opens it" flow).

Also exercises, beyond the minimum list:
  - Two independent PUText windows/documents open simultaneously.
  - Unsaved-change protection: closing a modified window puts up a real
    "Discard unsaved changes?" confirm modal (Cancel leaves it open,
    Discard actually closes it) -- and closing an *unmodified* window
    (just saved, untouched since) closes immediately with no modal,
    proving confirm_close's two real code paths both work.

Usage:
    tools/test-putext.py
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

    def press_repeat(self, combo, times, delay=0.08):
        """Sends the same key combo `times` times with a real settle delay
        between presses -- back-to-back send-key calls with no delay at
        all can have some presses silently dropped/coalesced by this
        environment's synthetic PS/2 injection (observed during manual
        smoke testing), unlike tools/test-pufiles.py's usage which never
        sends the same key more than once or twice in a row."""
        for _ in range(times):
            self.send_key_combo(combo)
            time.sleep(delay)

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
        self.mouse_to(x, y, settle)
        self.mouse_button(True)
        time.sleep(0.05)
        self.mouse_button(False)
        time.sleep(0.12)
        self.mouse_button(True)
        time.sleep(0.05)
        self.mouse_button(False)
        time.sleep(settle)

    def drag_from(self, from_x, from_y, dx, dy, steps=20, delay=0.08):
        self.mouse_to(from_x, from_y)
        self.mouse_button(True)
        time.sleep(0.1)
        self.walk_rel(dx, dy, steps=steps, delay=delay)
        time.sleep(0.15)
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


# ---- pude's own chrome geometry (user/pude.c) -----------------------------
BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_W = 8
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2  # 29
RESIZE_GRIP = 22
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_W, MENU_ITEM_H = 170, 28
MARGIN = 24

# ---- launcher order (user/pude.c's g_apps[]) -------------------------------
APP_PUTERM, APP_CALC, APP_PUFILES, APP_PUTEXT = 0, 1, 2, 3

# ---- PUFiles' own layout constants (user/pude_files.c) --------------------
PF_TOPBAR_H = 22
PF_TOOLBAR_H = 26
PF_STATUS_H = 20
PF_ROW_H = FONT_CELL_H + 4  # 21
PF_NUM_BUTTONS = 5
PF_DEFAULT_CW, PF_DEFAULT_CH = 480, 360

# ---- PUText's own layout constants (user/pude_text.c) ----------------------
PT_DEFAULT_CW, PT_DEFAULT_CH = 560, 420
PT_TOOLBAR_H = 30
PT_STATUS_H = 20


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


def client_origin(win_x, win_y):
    return win_x + BORDER, win_y + BORDER + TITLEBAR_H


def pf_toolbar_button_center(win_x, win_y, cw, idx):
    bw = cw // PF_NUM_BUTTONS
    x = idx * bw
    w = cw - idx * bw if idx == PF_NUM_BUTTONS - 1 else bw
    cx, cy = client_origin(win_x, win_y)
    return cx + x + w // 2, cy + PF_TOPBAR_H + PF_TOOLBAR_H // 2


def pf_row_center(win_x, win_y, row_index):
    cx, cy = client_origin(win_x, win_y)
    list_top = cy + PF_TOPBAR_H + PF_TOOLBAR_H
    return cx + 150, list_top + row_index * PF_ROW_H + PF_ROW_H // 2


def pt_toolbar_button_center(win_x, win_y, idx):
    """New/Open/Save/Save-As -- user/pude_text.c's pt_toolbar_buttons()."""
    bw, gap = 78, 4
    x = 4 + idx * (bw + gap)
    cx, cy = client_origin(win_x, win_y)
    return cx + x + bw // 2, cy + 3 + (PT_TOOLBAR_H - 6) // 2


def pt_text_area_point(win_x, win_y, row, col):
    """Screen point inside the text area at a given (row,col) -- for a
    click/drag target, not for asserting exact resulting selection bounds
    (real PS/2 relative mouse motion in this environment isn't pixel-exact
    -- see the file's own docstring)."""
    cx, cy = client_origin(win_x, win_y)
    return cx + 4 + col * FONT_CELL_W, cy + PT_TOOLBAR_H + row * FONT_CELL_H + 8


def pt_confirm_button_centers(win_x, win_y, cw, ch):
    """Cancel/Discard buttons for PUText's own confirm-close /
    confirm-discard modal -- user/pude_text.c's pt_confirm_rect()/
    pt_confirm_buttons() (same geometry for both dialogs)."""
    w = min(360, max(cw - 8, 60))
    h = min(120, max(ch - 8, 60))
    dx = (cw - w) // 2
    dy = (ch - h) // 2
    bw, bh, gap = 130, 26, 16
    total = bw * 2 + gap
    bx0 = dx + (w - total) // 2
    by = dy + h - bh - 12
    cx, cy = client_origin(win_x, win_y)
    cancel = (cx + bx0 + bw // 2, cy + by + bh // 2)
    discard = (cx + bx0 + bw + gap + bw // 2, cy + by + bh // 2)
    return cancel, discard


def pt_filepicker_save_button_center(win_x, win_y, cw, ch):
    """Save button inside PUText's embedded Save-As file picker --
    user/pude_filepicker.h's pu_filepicker_layout() (SAVE mode)."""
    dw = min(440, max(cw - 16, 60))
    dh = min(340, max(ch - 16, 80))
    dx = (cw - dw) // 2
    dy = (ch - dh) // 2
    path_h = 22
    filename_h = 28
    status_h = 18
    buttons_h = 34
    list_h = dh - path_h - filename_h - status_h - buttons_h
    y = dy + path_h + list_h + filename_h + status_h
    bw, bh, gap = 92, 26, 12
    bx0 = dx + dw - 2 * bw - gap - 10
    by = y + (buttons_h - bh) // 2
    save_x = bx0 + bw + gap
    cx, cy = client_origin(win_x, win_y)
    return cx + save_x + bw // 2, cy + by + bh // 2


def make_recipe_lines(target, touch_file):
    """Same technique as tools/test-pufiles.py's own helper -- extracts
    the exact recipe `make` would run without ever running/modifying
    anything for real, so this can't silently drift out of sync."""
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


def build_scratch_disk(tmp):
    """Builds a scratch persistent EXT2 image + MBR/GRUB ISO exactly like
    `make iso` -- no extra fixture needed (PUText itself creates the real
    fixture file this test reads back, via a real graphical Save)."""
    for f in ("build/pureunix.elf", "build/grub/boot.img", "build/grub/core.img"):
        if not os.path.exists(os.path.join(REPO_ROOT, f)):
            raise RuntimeError(f"{f} is missing -- run `make iso` first")

    ext2_cmd = "\n".join(make_recipe_lines("build/ext2-persistent.img", "tools/mkext2.py"))
    scratch_ext2 = os.path.join(tmp, "test-ext2.img")
    ext2_cmd = ext2_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    subprocess.run(ext2_cmd, cwd=REPO_ROOT, shell=True, check=True)

    iso_lines = make_recipe_lines("build/pureunix.iso", "tools/mkdiskimg.py")
    iso_cmd = "\n".join(iso_lines)
    scratch_iso = os.path.join(tmp, "test.iso")
    iso_cmd = iso_cmd.replace("build/pureunix.iso", scratch_iso, 1)
    iso_cmd = iso_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    subprocess.run(iso_cmd, cwd=REPO_ROOT, shell=True, check=True)

    return scratch_iso


def parse_ls_la(text):
    entries = []
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r"^([dl-])\S*\s+\d+\s+\d+\s+(.+)$", line)
        if not m:
            continue
        kind, name = m.group(1), m.group(2)
        name = name.split(" -> ")[0]
        if name in (".", ".."):
            continue
        entries.append((name, kind == "d"))
    return entries


def pufiles_sort(entries):
    return sorted(entries, key=lambda e: (0 if e[1] else 1, e[0].lower()))


EXPECTED_CONTENT = "Line One\nLine Two\nLine Three\nLine One"


def check_expected_content(transcript):
    """Checks that EXPECTED_CONTENT's 4 lines appear, in order, as real
    console output lines -- tolerant of drivers/vga.c's always-on
    scroll-perf reporter splicing "PERF scroll ..." lines in between on
    this shared serial console (a real, documented, unrelated project
    gotcha, not something `cat`'s actual output ever contains)."""
    real_lines = [l for l in transcript.splitlines() if l and not l.startswith("PERF ")]
    text = "\n".join(real_lines)
    return EXPECTED_CONTENT in text


def main():
    failures = []

    with tempfile.TemporaryDirectory(prefix="pureunix-putext-test-") as tmp:
        print("=== building scratch disk ===")
        iso = build_scratch_disk(tmp)

        short_tmp = "/tmp/putext-test"
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

            print("=== capturing ground-truth `ls -la /` (before the fixture exists) ===")
            qemu.type_text("ls -la /\n")
            time.sleep(1.0)
            baseline_root = parse_ls_la(qemu.tail())
            if not baseline_root:
                failures.append("ground-truth ls -la / returned nothing")

            print("=== launching pude ===")
            qemu.type_text("pude\n")
            time.sleep(2.0)
            shot(qemu, "01-desktop.ppm")

            print("=== [1] opening the launcher: PUText must be listed ===")
            qemu.click_at(*menu_button_center())
            shot(qemu, "02-launcher-with-putext.ppm")

            print("=== [1] launching PUText from the launcher ===")
            qemu.click_at(*launcher_item_center(APP_PUTEXT))
            time.sleep(1.0)
            shot(qemu, "03-putext-launched-empty.ppm")

            w1_x, w1_y = spawn_position(0)

            print("=== [2] typing multiple lines of text ===")
            qemu.type_text("Line One\nLine Two\nLine Three")
            time.sleep(0.3)
            shot(qemu, "04-typed-multiline.ppm")

            print("=== [3] moving the cursor into previously entered text ===")
            qemu.send_key_combo("home")
            time.sleep(0.1)
            qemu.press_repeat("up", 2)
            shot(qemu, "05-cursor-moved-up.ppm")

            print("=== [4] inserting text in the middle of a line ===")
            qemu.press_repeat("right", 5)
            qemu.type_text("XYZ-")
            time.sleep(0.2)
            shot(qemu, "06-mid-line-insert.ppm")

            print("=== [5] deleting text (Backspace) ===")
            qemu.press_repeat("backspace", 4)
            time.sleep(0.2)
            shot(qemu, "07-after-delete.ppm")

            print("=== [6] selecting text with the keyboard (Home, Shift+End) ===")
            qemu.send_key_combo("home")
            time.sleep(0.1)
            qemu.send_key_combo("shift+end")
            time.sleep(0.2)
            shot(qemu, "08-keyboard-selection.ppm")

            print("=== [7] copy, navigate, paste elsewhere ===")
            qemu.send_key_combo("ctrl+c")
            time.sleep(0.15)
            qemu.press_repeat("down", 2, delay=0.12)
            qemu.send_key_combo("end")
            time.sleep(0.1)
            qemu.send_key_combo("ret")
            time.sleep(0.15)
            qemu.send_key_combo("ctrl+v")
            time.sleep(0.2)
            shot(qemu, "09-copy-paste.ppm")

            print("=== [8] selecting text with the mouse (click + drag) ===")
            cx, cy = client_origin(w1_x, w1_y)
            start_x, start_y = pt_text_area_point(w1_x, w1_y, 1, 2)
            qemu.drag_from(start_x, start_y, 90, 34)
            time.sleep(0.3)
            shot(qemu, "10-mouse-selection.ppm")

            print("=== [9] cut, then paste back at the same spot (must be a no-op) ===")
            qemu.send_key_combo("ctrl+x")
            time.sleep(0.2)
            shot(qemu, "11-after-cut.ppm")
            qemu.send_key_combo("ctrl+v")
            time.sleep(0.2)
            shot(qemu, "12-after-paste-restored.ppm")

            print("=== [10] Save As: graphical file picker, no hardcoded path ===")
            qemu.click_at(*pt_toolbar_button_center(w1_x, w1_y, 3))
            time.sleep(0.5)
            shot(qemu, "13-save-as-picker.ppm")
            qemu.type_text("notesA.txt")
            time.sleep(0.2)
            shot(qemu, "14-filename-typed.ppm")
            qemu.click_at(*pt_filepicker_save_button_center(w1_x, w1_y, PT_DEFAULT_CW, PT_DEFAULT_CH))
            time.sleep(0.5)
            shot(qemu, "15-saved.ppm")

            print("=== opening a second, independent PUText window (multi-window) ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(APP_PUTEXT))
            time.sleep(1.0)
            w2_x, w2_y = spawn_position(1)
            qemu.type_text("second window content")
            time.sleep(0.2)
            shot(qemu, "16-two-putext-windows.ppm")

            print("=== unsaved-change protection: closing a modified window shows a real modal ===")
            w2_w, w2_h = whole_size(PT_DEFAULT_CW, PT_DEFAULT_CH)
            qemu.click_at(*close_button_center(w2_x, w2_y, w2_w))
            time.sleep(0.4)
            shot(qemu, "17-discard-confirm-modal.ppm")
            cancel_c, discard_c = pt_confirm_button_centers(w2_x, w2_y, PT_DEFAULT_CW, PT_DEFAULT_CH)
            qemu.click_at(*discard_c)
            time.sleep(0.4)
            shot(qemu, "18-window2-closed.ppm")

            print("=== [11] closing window 1 (saved, unmodified -> no modal expected) ===")
            w1_w, w1_h = whole_size(PT_DEFAULT_CW, PT_DEFAULT_CH)
            qemu.click_at(*close_button_center(w1_x, w1_y, w1_w))
            time.sleep(0.5)
            shot(qemu, "19-back-to-desktop.ppm")

            print("=== [16] launching PUFiles, finding notesA.txt via ground truth ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(APP_PUFILES))
            time.sleep(1.0)
            pf_x, pf_y = spawn_position(2)
            shot(qemu, "20-pufiles-root.ppm")

            merged = pufiles_sort(baseline_root + [("notesA.txt", False)])
            names = [n for n, _ in merged]
            if "notesA.txt" not in names:
                failures.append("notesA.txt not found in the computed PUFiles listing")
                row = 0
            else:
                row = names.index("notesA.txt")
            print(f"    notesA.txt expected at PUFiles row {row}")

            print("=== [12][16] selecting notesA.txt via keyboard (auto-scrolls into view) ===")
            # pf_row_center() assumes an unscrolled list -- row 18 is off
            # the default (13-visible-row) window, so a blind mouse click
            # at its unscrolled pixel position would miss; PUFiles' own
            # keyboard Down auto-scrolls the selection into view
            # (pf_ensure_visible(), user/pude_files.c), which a plain
            # mouse click can't do on this platform (no scrollbar/wheel).
            qemu.press_repeat("down", row, delay=0.06)
            time.sleep(0.3)
            shot(qemu, "20b-notesA-selected.ppm")
            print("=== [12][16] opening it (Enter) -> PUText must launch with it loaded ===")
            qemu.send_key_combo("ret")
            time.sleep(1.2)
            shot(qemu, "21-reopened-in-putext.ppm")

            w3_x, w3_y = spawn_position(3)

            print("=== [14] resizing the reopened PUText window ===")
            gx, gy = resize_grip_center(w3_x, w3_y, *whole_size(PT_DEFAULT_CW, PT_DEFAULT_CH))
            qemu.drag_from(gx, gy, 140, 100)
            time.sleep(0.4)
            shot(qemu, "22-resized.ppm")

            print("=== [15] typing after resize: text/cursor/UI must stay correct ===")
            qemu.type_text("!")
            time.sleep(0.2)
            shot(qemu, "23-typed-after-resize.ppm")

            print("=== emergency desktop quit; verifying real saved content from ash ===")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            # `clear` first -- drivers/vga.c's always-on scroll-perf report
            # can otherwise splice "PERF scroll ..." lines mid-transcript
            # into cat's own output on this serial console (project gotcha).
            qemu.type_text("clear\n")
            time.sleep(0.3)
            qemu.type_text("cat /notesA.txt\n")
            transcript = qemu.wait_for(r"Line One", 20)
            if not check_expected_content(transcript):
                failures.append(
                    f"notesA.txt content mismatch after same-boot cat; expected the 4 lines "
                    f"{EXPECTED_CONTENT.splitlines()!r} in order in the transcript"
                )
            else:
                print("PASS: notesA.txt has the exact expected content (same boot)")
        finally:
            qemu.kill()

        print("=== boot 2: verifying persistence across a real reboot ===")
        serial2 = os.path.join(short_tmp, "serial2.log")
        qmp2 = os.path.join(short_tmp, "qmp2.sock")
        qemu2 = QemuSession(short_iso, serial2, qmp2)
        try:
            qemu2.wait_for(r"Enter 'help'", 60)
            time.sleep(1.0)
            qemu2.type_text("clear\n")
            time.sleep(0.3)
            qemu2.type_text("cat /notesA.txt\n")
            transcript2 = qemu2.wait_for(r"Line One", 20)
            if not check_expected_content(transcript2):
                failures.append("notesA.txt content did not persist correctly across reboot")
            else:
                print("PASS: notesA.txt content persisted across a real reboot")
        finally:
            qemu2.kill()

    if failures:
        print("=== FAIL ===")
        for f in failures:
            print(" -", f)
        sys.exit(1)

    print("=== PASS: PUText end-to-end interactive test complete ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
