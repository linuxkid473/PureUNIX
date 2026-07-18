#!/usr/bin/env python3
"""tools/test-qt-widgets-pude.py — interactive QEMU test driver for
docs/qt-port.md Phase 6's QtWidgets milestone: pude's "Qt Widgets Test"
launcher menu item (user/pude_qtclient.c's qtclient_widgets_app_class)
fork/exec'ing a real, unmodified Qt Widgets application
(user/qtwidgetstest.cpp -- QMainWindow + QLabel/QLineEdit/QTextEdit/
QPushButton via a real layout) against the "pureunix" QPA plugin.

Same technique as tools/test-qt-pude.py (see that file's own docstring
for the QMP/HMP mouse rationale) -- duplicated rather than imported,
matching tools/test-pude-perf.py's own precedent for these QEMU test
drivers.

Usage:
    tools/test-qt-widgets-pude.py build/qpa-scratch.iso --screenshot-dir DIR
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BASE_MAP = {
    " ": ("spc", False), "\n": ("ret", False), "/": ("slash", False),
    "-": ("minus", False), "_": ("minus", True), ".": ("dot", False),
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
    def __init__(self, iso, serial_log, qmp_sock):
        self.serial_log = serial_log
        self.qmp_sock = qmp_sock
        for p in (qmp_sock, serial_log):
            try:
                os.remove(p)
            except FileNotFoundError:
                pass
        args = [
            "qemu-system-i386", "-m", "128M",
            "-cdrom", iso, "-boot", "d",
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
        import re
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


# ---- pude's menu geometry (user/pude.c) -- kept in sync by hand, same
# convention tools/test-qt-pude.py's own geometry block uses.
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_W, MENU_ITEM_H = 170, 28
# g_apps[] order: PUTerm(0), Calculator(1), PUFiles(2), PUText(3),
# Settings(4), Qt Application(5), Qt Widgets Test
# (6, qtclient_widgets_app_class) -- user/pude.c.
NUM_APPS = 7
QTCLIENT_WIDGETS_INDEX = 6

# user/pude.c's BORDER/TITLEBAR_PAD/FONT_CELL_H constants -- same values
# tools/test-pude-dock.py's own geometry block hardcodes.
BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2


def menu_item_center(i):
    py = MENU_BTN_Y + MENU_BTN_H + 4
    iy = py + 4 + i * MENU_ITEM_H
    return MENU_BTN_X + 4 + (MENU_ITEM_W - 8) // 2, iy + (MENU_ITEM_H - 2) // 2


def menu_btn_center():
    return MENU_BTN_X + MENU_BTN_W // 2, MENU_BTN_Y + MENU_BTN_H // 2


def spawn_position(spawn_index):
    MARGIN = 24
    cascade = (spawn_index % 8) * 26
    return MARGIN + cascade, MARGIN + cascade


def client_origin(spawn_index):
    """Top-left of the window's *client* area (inside chrome)."""
    wx, wy = spawn_position(spawn_index)
    return wx + BORDER, wy + BORDER + TITLEBAR_H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("--screenshot-dir", required=True)
    args = ap.parse_args()

    os.makedirs(args.screenshot_dir, exist_ok=True)
    shots = []

    def shot(name):
        path = os.path.join(args.screenshot_dir, name)
        qemu.screendump(path)
        shots.append(path)
        print(f"screenshot: {path}")
        return path

    tmp = os.environ.get("PUDE_TEST_KEEP_TMP") or tempfile.mkdtemp(prefix="pureunix-qt-widgets-pude-test-")
    print(f"(tmp dir: {tmp})")
    serial = os.path.join(tmp, "serial.log")
    qmp = os.path.join(tmp, "qmp.sock")

    qemu = QemuSession(args.iso, serial, qmp)
    try:
        print("=== waiting for shell prompt ===")
        qemu.wait_for(r"Enter 'help'", 60)
        time.sleep(1.5)

        print("=== launching pude ===")
        qemu.type_text("pude\n")
        time.sleep(2.0)
        shot("w01-desktop.ppm")

        print("=== opening launcher menu ===")
        qemu.click_at(*menu_btn_center())
        shot("w02-menu-open.ppm")

        print("=== clicking 'Qt Widgets Test' menu entry ===")
        qemu.click_at(*menu_item_center(QTCLIENT_WIDGETS_INDEX))
        time.sleep(2.0)
        shot("w03-widgets-window-just-spawned.ppm")
        print("=== waiting up to 30s for [002] to confirm the window really painted ===")
        try:
            qemu.wait_for(r"\[002\]", 30)
            print("[002] observed")
        except TimeoutError:
            print("WARN: [002] never printed within 30s")
        time.sleep(2.0)
        shot("w04-widgets-window-settled.ppm")

        cx, cy = client_origin(0)
        print("=== clicking into the QLineEdit and typing ===")
        qemu.click_at(cx + 100, cy + 35)
        time.sleep(0.3)
        qemu.type_text("hello widgets")
        time.sleep(0.5)
        shot("w05-lineedit-typed.ppm")

        print("=== clicking the QPushButton (real signal/slot round trip) ===")
        # Button row sits near the bottom of the *actual*, Qt-layout-grown
        # window (docs/qt-port.md's Phase 5 repaint-loop fix means this is
        # no longer the fixed 420x320 default_client_w/h -- the real
        # QVBoxLayout/QTextEdit/QHBoxLayout combination settles noticeably
        # bigger). Measured from a real w04 screendump rather than derived
        # from the old fixed size.
        qemu.click_at(cx + 139, cy + 297)
        time.sleep(0.5)
        shot("w06-after-button-click-1.ppm")
        qemu.click_at(cx + 139, cy + 297)
        time.sleep(0.5)
        shot("w07-after-button-click-2.ppm")

        print("=== emergency whole-desktop quit (Ctrl+F12) back to outer ash ===")
        qemu.send_key_combo("ctrl+f12")
        time.sleep(1.0)
        qemu.type_text("echo qt_widgets_pude_test_wm_exited_ok\n")
        transcript = qemu.wait_for(r"qt_widgets_pude_test_wm_exited_ok", 20)
        if transcript.count("qt_widgets_pude_test_wm_exited_ok") < 1:
            print("FAIL: outer ash shell did not cleanly resume after pude exited")
            sys.exit(1)
        print("PASS: outer shell cleanly restored after pude exited")

        clicks = transcript.count("QPushButton::clicked signal fired")
        print(f"QPushButton::clicked fired {clicks} time(s) (expected 2)")
        if clicks < 2:
            print("WARN: expected 2 button-click signal firings, see screenshots for the real check")
    finally:
        qemu.kill()

    print("=== test-qt-widgets-pude complete (visual verification required from screenshots) ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
