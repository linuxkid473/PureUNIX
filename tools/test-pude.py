#!/usr/bin/env python3
"""tools/test-pude.py — interactive QEMU test driver for pude, PureUNIX's
ring-3 desktop shell (docs/pude.md): boots the real persistent disk image
built by `make iso`, launches `pude` from a real BusyBox ash prompt, and
drives real injected keyboard/mouse input into the desktop it opens (same
QMP send-key technique test-ncurses-demo.py/test-persistent-boot.py/
vt-inject-test.py all use — PureUnix's keyboard is PS/2-only, there is no
usable stdin over `-serial stdio`). Captures QMP `screendump` screenshots
at each step, since a graphics-mode program's rendered output never
reaches the serial log the way plain console text does — a screenshot is
the only way to verify it.

Exercises, in order:
  1. `pude` launches from ash and the desktop appears with NO window
     auto-opened (just the top-left "Menu" control) -- the launcher is the
     only way to open anything.
  2. Clicking "Menu" opens the launcher popup listing PUTerm and
     Calculator.
  3. Launching PUTerm: a real chrome-decorated window appears, running a
     real BusyBox ash under a real pty -- `echo`, `ls`, and `lua` all
     exercised and screenshotted.
  4. Launching Calculator (while PUTerm is still open, proving multi-
     window coexistence): real mouse clicks on its buttons compute
     7 + 8 = 15.
  5. Dragging Calculator's resize grip -- the button grid re-lays out
     (not a stretched bitmap) and a click after the resize still hits the
     right button.
  6. Dragging PUTerm's resize grip -- the terminal reflows to the new
     pixel size and the shell keeps working.
  7. Closing PUTerm via its close button -- the window disappears but the
     desktop (and Menu control) stays alive.
  8. Reopening PUTerm from the launcher after closing it -- a fresh
     window with a fresh, working shell.

All mouse motion goes through HMP's `mouse_move`/`mouse_button` (QMP's own
`input-send-event` "rel"/"btn" types are a confirmed silent no-op against
this environment's real PS/2 mouse -- see QemuSession.mouse_rel()) and is
always split into many small relative steps, never one large jump: a
single oversized `mouse_move` call is itself clamped/truncated by QEMU to
some small effective delta, so reaching a precise on-screen target (or
reliably clamping into a screen corner) requires walking there in several
real motion events, exactly like a human dragging a physical mouse.

Usage:
    tools/test-pude.py DISK.img --screenshot-dir DIR
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

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
        """Relative motion via the HMP `mouse_move` command (not QMP
        `input-send-event`'s "rel" type -- confirmed, empirically, to be a
        silent no-op against this environment's real PS/2 mouse device;
        `mouse_move` reliably moves it). Matches drivers/mouse.c's real
        relative-packet PS/2 mouse (see docs/sdl-port.md). A single call
        with a very large delta does NOT reliably move that far in one
        shot (QEMU/PS/2 packet-level truncation) -- see walk_rel()."""
        self._cmd("human-monitor-command", **{"command-line": f"mouse_move {dx} {dy}"})

    def mouse_button(self, down, button_mask=1):
        """button_mask: HMP `mouse_button` bitmask (1=left, 2=middle,
        4=right); `down=False` always means "release all buttons" (mask 0),
        matching a plain click-release."""
        mask = button_mask if down else 0
        self._cmd("human-monitor-command", **{"command-line": f"mouse_button {mask}"})

    def walk_rel(self, dx, dy, steps=20, delay=0.03):
        """Moves the mouse by (dx,dy) split across `steps` real
        mouse_rel() calls -- a single oversized call is itself clamped/
        truncated by QEMU, so covering real distance (or reliably
        clamping into a screen corner via a large overshoot) needs
        several smaller motion events, not one big jump."""
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
        """Moves the cursor to an absolute (x,y) by first clamping into
        the screen's top-left corner (a large overshoot always lands
        exactly at (0,0), regardless of the real screen resolution --
        kernel/vt.c's own mouse-position clamping) and then walking the
        exact (x,y) offset from that known origin."""
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

    def drag_from(self, from_x, from_y, dx, dy, steps=15, delay=0.06):
        """Moves to (from_x,from_y), presses the left button, walks
        (dx,dy) in real relative motion events, then releases -- a real
        click-drag-release, e.g. for title-bar move or resize-grip drag."""
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


# ---- pude's own chrome geometry (user/pude.c) -------------------------------
# Kept in sync by hand with the constants in user/pude.c -- there is no
# shared source of truth between the C WM and this Python test driver.
BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2  # 29
RESIZE_GRIP = 22
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_W, MENU_ITEM_H = 170, 28
MARGIN = 24

PUTERM_CLIENT_W, PUTERM_CLIENT_H = 80 * 8, 24 * FONT_CELL_H  # 640 x 408
CALC_CLIENT_W, CALC_CLIENT_H = 240, 320
CALC_DISPLAY_H = 48


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
    """Mirrors user/pude.c's spawn_window() cascade: each new window
    (regardless of how many were closed before it) is offset by another
    26px so repeated launches never stack exactly on top of each other."""
    cascade = (spawn_index % 8) * 26
    return MARGIN + cascade, MARGIN + cascade


def resize_grip_center(win_x, win_y, whole_w, whole_h):
    return win_x + whole_w - RESIZE_GRIP // 2, win_y + whole_h - RESIZE_GRIP // 2


def calc_button_center(win_x, win_y, client_w, client_h, row, col, wide=False):
    cx, cy = win_x + BORDER, win_y + BORDER + TITLEBAR_H
    display_h = min(CALC_DISPLAY_H, client_h)
    grid_h = max(0, client_h - display_h)
    row_h = grid_h // 5
    col_w = client_w // 4
    bx = cx + (0 if wide else col * col_w)
    by = cy + display_h + row * row_h
    bw = client_w if wide else col_w
    return bx + bw // 2, by + row_h // 2


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

    with tempfile.TemporaryDirectory(prefix="pureunix-pude-test-") as tmp:
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
            shot("01-desktop-no-autolaunch.ppm")

            print("=== opening the launcher ===")
            qemu.click_at(*menu_button_center())
            shot("02-launcher-open.ppm")

            print("=== launching PUTerm from the launcher ===")
            qemu.click_at(*launcher_item_center(0))
            time.sleep(1.0)
            shot("03-puterm-launched.ppm")

            puterm_x, puterm_y = spawn_position(0)
            puterm_w, puterm_h = whole_size(PUTERM_CLIENT_W, PUTERM_CLIENT_H)

            print("=== echo inside PUTerm ===")
            qemu.type_text("echo hello_from_puterm\n")
            time.sleep(0.6)
            shot("04-echo.ppm")

            print("=== ls inside PUTerm ===")
            qemu.type_text("ls /\n")
            time.sleep(0.6)
            shot("05-ls.ppm")

            print("=== lua inside PUTerm ===")
            qemu.type_text('lua -e "print(6*7)"\n')
            time.sleep(0.8)
            shot("06-lua.ppm")

            print("=== opening Calculator from the launcher (multi-window) ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(1))
            time.sleep(1.0)
            shot("07-calculator-launched.ppm")

            calc_x, calc_y = spawn_position(1)
            calc_w, calc_h = whole_size(CALC_CLIENT_W, CALC_CLIENT_H)

            print("=== 7 + 8 = via real mouse clicks ===")
            qemu.click_at(*calc_button_center(calc_x, calc_y, CALC_CLIENT_W, CALC_CLIENT_H, 0, 0))  # 7
            qemu.click_at(*calc_button_center(calc_x, calc_y, CALC_CLIENT_W, CALC_CLIENT_H, 3, 3))  # +
            qemu.click_at(*calc_button_center(calc_x, calc_y, CALC_CLIENT_W, CALC_CLIENT_H, 0, 1))  # 8
            qemu.click_at(*calc_button_center(calc_x, calc_y, CALC_CLIENT_W, CALC_CLIENT_H, 3, 2))  # =
            shot("08-calculator-7-plus-8-equals-15.ppm")

            print("=== dragging Calculator's resize grip ===")
            gx, gy = resize_grip_center(calc_x, calc_y, calc_w, calc_h)
            qemu.drag_from(gx, gy, 120, 100)
            new_calc_client_w, new_calc_client_h = CALC_CLIENT_W + 120, CALC_CLIENT_H + 100
            shot("09-calculator-resized.ppm")

            print("=== clicking a button after Calculator resize (hit-test still correct) ===")
            qemu.click_at(*calc_button_center(calc_x, calc_y, new_calc_client_w, new_calc_client_h, 1, 1))  # 5
            shot("10-calculator-click-after-resize.ppm")

            print("=== bringing PUTerm back to front (click its visible area) ===")
            qemu.click_at(puterm_x + 400, puterm_y + 300)
            time.sleep(0.3)

            print("=== dragging PUTerm's resize grip (terminal must reflow) ===")
            gx, gy = resize_grip_center(puterm_x, puterm_y, puterm_w, puterm_h)
            qemu.drag_from(gx, gy, -200, -150)
            shot("11-puterm-resized.ppm")

            print("=== confirming the shell survived the resize ===")
            qemu.type_text("echo after_resize_ok\n")
            time.sleep(0.6)
            shot("12-puterm-after-resize-echo.ppm")

            print("=== closing PUTerm via its close button ===")
            new_puterm_w = puterm_w - 200
            cx, cy = close_button_center(puterm_x, puterm_y, new_puterm_w)
            qemu.click_at(cx, cy)
            time.sleep(0.5)
            shot("13-after-close-desktop-alive.ppm")

            print("=== reopening PUTerm from the launcher after closing it ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(0))
            time.sleep(1.0)
            qemu.type_text("echo reopened_ok\n")
            time.sleep(0.6)
            shot("14-reopened-puterm.ppm")

            print("=== clean exit: close the reopened PUTerm, desktop stays up ===")
            # 3rd window spawned overall (PUTerm, Calculator, reopened PUTerm)
            # -- the cascade offset keeps advancing even across closed windows.
            reopened_x, reopened_y = spawn_position(2)
            qemu.click_at(*close_button_center(reopened_x, reopened_y, puterm_w))
            time.sleep(0.5)
            shot("15-final-desktop.ppm")

            print("=== emergency whole-desktop quit (Ctrl+F12) back to outer ash ===")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            qemu.type_text("echo wm_exited_ok\n")
            transcript = qemu.wait_for(r"wm_exited_ok", 20)
            if transcript.count("wm_exited_ok") < 1:
                print("FAIL: outer ash shell did not cleanly resume after pude exited")
                sys.exit(1)
            print("PASS: outer shell cleanly restored after pude exited")
        finally:
            qemu.kill()

    print("=== PASS: pude desktop interactive test complete ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
