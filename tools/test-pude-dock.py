#!/usr/bin/env python3
"""tools/test-pude-dock.py — interactive QEMU test driver for pude's dock
and app drawer (docs/pude.md's "Dock and app drawer" section). Boots the
real persistent disk image built by `make iso`, launches `pude` from a
real BusyBox ash prompt, and drives real injected mouse input into the
desktop shell's new icon-driven launch surfaces (same QMP/HMP technique
tools/test-pude.py uses -- see that file's module docstring for why HMP
mouse_move/mouse_button, not QMP input-send-event, is the only thing that
actually moves this environment's real PS/2 mouse). Captures QMP
`screendump` screenshots at each step for visual verification (stale
pixels, icon legibility, cursor trails, z-order).

Exercises, in order:
  1. The dock is visible on the plain desktop with its five pinned icons
     (PUTerm, Calculator, PUFiles, PUText, Settings) -- no auto-opened
     window. Only the original four are individually launched/verified
     below; Settings has its own dedicated driver, tools/test-pude-
     settings.py.
  2. Hovering each dock icon in turn (visual-only, screenshotted).
  3. Launching PUText, Calculator, PUFiles, and PUTerm from the dock, one
     at a time -- each produces a real chrome-decorated window using the
     same app_class_t the existing launcher menu already spawns (multi-
     window coexistence, since earlier launches stay open).
  4. Closing all four via their close buttons -- desktop and dock survive.
  5. Opening the app drawer via its graphical (grid-of-dots) button --
     an icon grid overlay appears, drawn above the (now empty) desktop.
  6. Hovering multiple drawer entries (visual-only).
  7. Launching PUText from the drawer -- the popup closes automatically
     and the requested window appears.
  8. Reopening the drawer, then clicking outside it: it closes, and the
     click does NOT activate/focus the window opened in step 7 (verified
     by confirming that window's title bar is still un-focused-colored
     and no new window was spawned).
  9. Opening the drawer again with the PUText window still open, to check
     the popup renders above it (z-order) rather than behind.

Usage:
    tools/test-pude-dock.py DISK.img --screenshot-dir DIR
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

# ---- QEMU/QMP session plumbing -- identical technique to tools/test-pude.py
# (duplicated rather than imported, matching tools/test-pude-perf.py's own
# precedent, so this script has no import-path dependency on another
# test's module layout).


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


# ---- pude's dock/drawer chrome geometry (user/pude.c) -----------------------
# Kept in sync by hand with the constants in user/pude.c -- there is no
# shared source of truth between the C WM and this Python test driver
# (same convention tools/test-pude.py's own geometry block already uses).
SCREEN_W, SCREEN_H = 1280, 800

DOCK_ICON = 44
DOCK_GAP = 10
DOCK_PAD = 10
DOCK_MARGIN_B = 14
NUM_PINNED = 5  # PUTerm, Calculator, PUFiles, PUText, Settings -- g_apps[]
                # order (docs/pude.md's "Settings" section) -- this test
                # only exercises the original four by index (0-3, unaffected
                # by Settings being appended last), but the dock/drawer are
                # centered on the *total* pinned/graphical count, so a
                # wrong NUM_PINNED here would silently mis-click every icon.

MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
DRAWER_BTN_X = MENU_BTN_X + MENU_BTN_W + 8
DRAWER_BTN_Y = MENU_BTN_Y
DRAWER_BTN_W = MENU_BTN_H
DRAWER_BTN_H = MENU_BTN_H

DRAWER_ICON = 64
DRAWER_CELL_W = 96
DRAWER_CELL_H = 96
DRAWER_COLS = 4
DRAWER_PAD = 16
NUM_DRAWER_APPS = 5  # same five, all graphical=true

BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2


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


def dock_empty_space():
    """A point on the dock's background bar that isn't over any icon --
    the gap between the last icon and the bar's right padding edge is too
    narrow at 4 icons, so this uses the gap between icons 0 and 1 isn't
    safe either (no gap wider than DOCK_GAP=10px exists there); instead
    use a point just inside the bar's own left padding strip, left of
    icon 0, which is guaranteed empty (DOCK_PAD=10px wide)."""
    dx, dy, _, dh = dock_rect()
    return dx + DOCK_PAD // 2, dy + dh // 2


def drawer_btn_center():
    return DRAWER_BTN_X + DRAWER_BTN_W // 2, DRAWER_BTN_Y + DRAWER_BTN_H // 2


def drawer_popup_rect(screen_w=SCREEN_W, screen_h=SCREEN_H):
    cols = DRAWER_COLS
    rows = max(1, (NUM_DRAWER_APPS + cols - 1) // cols)
    pw = min(DRAWER_PAD * 2 + cols * DRAWER_CELL_W, screen_w - 48)
    ph = min(DRAWER_PAD * 2 + rows * DRAWER_CELL_H, screen_h - 48)
    px = (screen_w - pw) // 2
    py = (screen_h - ph) // 2
    return px, py, pw, ph


def drawer_icon_center(i, screen_w=SCREEN_W, screen_h=SCREEN_H):
    px, py, _, _ = drawer_popup_rect(screen_w, screen_h)
    col = i % DRAWER_COLS
    row = i // DRAWER_COLS
    cx = px + DRAWER_PAD + col * DRAWER_CELL_W
    cy = py + DRAWER_PAD + row * DRAWER_CELL_H
    icon_x = cx + (DRAWER_CELL_W - DRAWER_ICON) // 2
    icon_y = cy
    return icon_x + DRAWER_ICON // 2, icon_y + DRAWER_ICON // 2


def outside_drawer_point(screen_w=SCREEN_W, screen_h=SCREEN_H):
    """A point guaranteed outside the popup (and outside the dock/menu/
    drawer button) -- used for the "click outside dismisses" check."""
    px, py, pw, ph = drawer_popup_rect(screen_w, screen_h)
    return px + pw + 40, py  # to the popup's right, same row


def spawn_position(spawn_index):
    """Mirrors user/pude.c's spawn_window() cascade -- see tools/test-
    pude.py's own copy of this helper for the full rationale."""
    MARGIN = 24
    cascade = (spawn_index % 8) * 26
    return MARGIN + cascade, MARGIN + cascade


def close_button_center(win_x, win_y, whole_w):
    bh = TITLEBAR_H - 2 * (TITLEBAR_PAD // 2 + 1)
    bx = win_x + whole_w - BORDER - TITLEBAR_PAD - 20
    by = win_y + BORDER + (TITLEBAR_H - bh) // 2
    return bx + 10, by + bh // 2


def whole_size(client_w, client_h):
    return client_w + 2 * BORDER, client_h + 2 * BORDER + TITLEBAR_H


PUTEXT_CLIENT_W, PUTEXT_CLIENT_H = 560, 420
CALC_CLIENT_W, CALC_CLIENT_H = 240, 320
PUFILES_CLIENT_W, PUFILES_CLIENT_H = 480, 360
PUTERM_CLIENT_W, PUTERM_CLIENT_H = 80 * 8, 24 * FONT_CELL_H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk_img")
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

    with tempfile.TemporaryDirectory(prefix="pureunix-pude-dock-test-") as tmp:
        serial = os.path.join(tmp, "serial.log")
        qmp = os.path.join(tmp, "qmp.sock")

        qemu = QemuSession(args.disk_img, serial, qmp)
        try:
            print("=== waiting for shell prompt ===")
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            print("=== launching pude ===")
            qemu.type_text("pude\n")
            time.sleep(2.0)
            shot("d01-desktop-with-dock.ppm")

            print("=== hovering each dock icon in turn ===")
            for i in range(NUM_PINNED):
                qemu.mouse_to(*dock_icon_center(i))
                shot(f"d02-dock-hover-{i}.ppm")

            print("=== clicking empty dock space (must not launch anything) ===")
            qemu.click_at(*dock_empty_space())
            shot("d03-dock-empty-click-noop.ppm")

            # g_apps[] / pinned_apps[] order: PUTerm(0), Calculator(1),
            # PUFiles(2), PUText(3) -- see user/pude.c.
            print("=== launching PUTerm from the dock ===")
            qemu.click_at(*dock_icon_center(0))
            time.sleep(1.0)
            shot("d04-puterm-from-dock.ppm")

            print("=== launching Calculator from the dock ===")
            qemu.click_at(*dock_icon_center(1))
            time.sleep(1.0)
            shot("d05-calculator-from-dock.ppm")

            print("=== launching PUFiles from the dock ===")
            qemu.click_at(*dock_icon_center(2))
            time.sleep(1.0)
            shot("d06-pufiles-from-dock.ppm")

            print("=== launching PUText from the dock ===")
            qemu.click_at(*dock_icon_center(3))
            time.sleep(1.0)
            shot("d07-putext-from-dock.ppm")

            print("=== confirming PUTerm (spawn 0) still works: echo ===")
            puterm_x, puterm_y = spawn_position(0)
            # PUTerm was spawned first, so every later cascade (Calculator,
            # PUFiles, PUText, each offset another 26px down-right) is drawn
            # on top of it and would swallow a click anywhere deep inside
            # its nominal client rect (topmost-first hit-testing, same as
            # tools/test-pude.py's own multi-window clicks always target a
            # point known NOT to be covered by a later window). PUTerm's
            # own title bar, close to its top-left corner, is the one strip
            # no later cascade step (starting at +26px each) ever reaches.
            qemu.click_at(puterm_x + 10, puterm_y + 15)
            time.sleep(0.3)
            qemu.type_text("echo dock_puterm_ok\n")
            time.sleep(0.6)
            shot("d08-puterm-still-works.ppm")

            print("=== closing all four dock-launched windows ===")
            # Close in current z-order, topmost first -- NOT plain reverse
            # spawn order. The previous step's click on PUTerm's title bar
            # (to focus it for typing) correctly raised PUTerm to the front
            # of the z-order, same as clicking any window's title bar
            # always does (user/pude.c's bring_to_front(), unchanged by the
            # dock/drawer). So the actual front-to-back order at this point
            # is PUTerm (just raised), then PUText, PUFiles, Calculator
            # (original spawn order) -- a window's close button is only
            # reliably clickable while it's the topmost thing covering that
            # screen point, since hit-testing is topmost-first.
            windows = [
                (0, PUTERM_CLIENT_W, PUTERM_CLIENT_H),
                (3, PUTEXT_CLIENT_W, PUTEXT_CLIENT_H),
                (2, PUFILES_CLIENT_W, PUFILES_CLIENT_H),
                (1, CALC_CLIENT_W, CALC_CLIENT_H),
            ]
            for idx, cw, ch in windows:
                wx, wy = spawn_position(idx)
                ww, _wh = whole_size(cw, ch)
                qemu.click_at(*close_button_center(wx, wy, ww))
                time.sleep(0.4)
            shot("d09-desktop-after-closing-all.ppm")

            print("=== opening the app drawer via its graphical icon ===")
            qemu.click_at(*drawer_btn_center())
            shot("d10-app-drawer-open.ppm")

            print("=== hovering multiple drawer entries ===")
            for i in range(NUM_DRAWER_APPS):
                qemu.mouse_to(*drawer_icon_center(i))
                shot(f"d11-drawer-hover-{i}.ppm")

            print("=== launching PUText from the drawer (spawn index 4) ===")
            qemu.click_at(*drawer_icon_center(3))
            time.sleep(1.0)
            shot("d12-putext-from-drawer.ppm")

            putext_x, putext_y = spawn_position(4)

            print("=== reopening the drawer over the now-open PUText window (z-order) ===")
            qemu.click_at(*drawer_btn_center())
            shot("d13-drawer-over-window.ppm")

            print("=== clicking outside the drawer: must dismiss without activating PUText ===")
            qemu.click_at(*outside_drawer_point())
            shot("d14-drawer-dismissed.ppm")

            print("=== confirming the dismissal click didn't spawn a duplicate/second window ===")
            # A click on plain empty desktop, clear of the PUText window,
            # the dock, and the menu/drawer buttons -- just to catch the
            # screen in its settled post-dismissal state (no separate
            # assertion here beyond the screenshot itself: a duplicate
            # window would already be visible in d14's screendump above).
            qemu.click_at(putext_x + 700, 200)
            time.sleep(0.3)
            shot("d15-final-desktop.ppm")

            print("=== emergency whole-desktop quit (Ctrl+F12) back to outer ash ===")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            qemu.type_text("echo dock_test_wm_exited_ok\n")
            transcript = qemu.wait_for(r"dock_test_wm_exited_ok", 20)
            if transcript.count("dock_test_wm_exited_ok") < 1:
                print("FAIL: outer ash shell did not cleanly resume after pude exited")
                sys.exit(1)
            print("PASS: outer shell cleanly restored after pude exited")
        finally:
            qemu.kill()

    print("=== PASS: pude dock/app-drawer test complete ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
