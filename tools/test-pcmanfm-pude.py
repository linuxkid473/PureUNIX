#!/usr/bin/env python3
"""tools/test-pcmanfm-pude.py — interactive QEMU test driver for
docs/pcmanfm-port.md's real end-to-end path: pude's "File Manager"
launcher menu item (user/pude_qtclient.c's qtclient_pcmanfm_app_class)
fork/exec'ing the real upstream PCManFM-Qt binary (/bin/pcmanfm-qt) via
the "pureunix" QPA plugin, then driving real injected keyboard/mouse
input into it -- same technique as tools/test-qt-pude.py.

Unlike test-qt-pude.py (which boots a small dedicated qpa-scratch.iso),
this boots the real, full build/pureunix-live.iso: pcmanfm-qt is a real
21 MB stripped binary, big enough that it only ever got wired into the
normal shared build/ext2.img (see Makefile's PCMANFM_QT_ELF and
tools/mkext2.py's NUM_GROUPS bump to 10), not a special-purpose scratch
image.

Usage:
    tools/test-pcmanfm-pude.py build/pureunix-live.iso --screenshot-dir DIR
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
    def __init__(self, iso, serial_log, qmp_sock, drive_boot=False):
        self.serial_log = serial_log
        self.qmp_sock = qmp_sock
        for p in (qmp_sock, serial_log):
            try:
                os.remove(p)
            except FileNotFoundError:
                pass
        if drive_boot:
            # build/pureunix.iso is a real MBR+GRUB+EXT2 disk image
            # (tools/mkdiskimg.py), not a LIVE_ISO GRUB-ramdisk-module ISO
            # -- boot it as an actual disk, matching the Makefile's own
            # `run:` target, so pcmanfm-qt's real EXT2 root partition
            # (bumped to --num-groups 10 to fit it, see Makefile's
            # DISK_PERSISTENT comment) is read via normal disk I/O rather
            # than loaded whole into RAM the way -cdrom's GRUB module path
            # would (and does hit a real RAM ceiling at this size).
            disk_args = ["-drive", f"file={iso},format=raw", "-boot", "c"]
        else:
            disk_args = ["-cdrom", iso, "-boot", "d"]
        args = [
            "qemu-system-i386", "-m", "128M",
            *disk_args,
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
# convention tools/test-pude-dock.py/test-qt-pude.py's own geometry
# blocks use.
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_W, MENU_ITEM_H = 170, 28
# g_apps[] order: PUTerm(0), Calculator(1), PUFiles(2), PUText(3),
# Settings(4), Qt Application(5, qtclient_app_class), Qt Widgets Test
# (6, qtclient_widgets_app_class), File Manager (7,
# qtclient_pcmanfm_app_class) -- user/pude.c.
NUM_APPS = 8
PCMANFM_INDEX = 7


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("--screenshot-dir", required=True)
    ap.add_argument("--drive-boot", action="store_true",
                     help="boot as a real disk (-drive/-boot c) instead of "
                          "a LIVE_ISO GRUB-module CD (-cdrom/-boot d) -- "
                          "use for build/pureunix.iso (tools/mkdiskimg.py's "
                          "real MBR+GRUB+EXT2 image), not build/pureunix-"
                          "live.iso")
    args = ap.parse_args()

    os.makedirs(args.screenshot_dir, exist_ok=True)
    shots = []

    def shot(name):
        path = os.path.join(args.screenshot_dir, name)
        qemu.screendump(path)
        shots.append(path)
        print(f"screenshot: {path}")
        return path

    tmp = os.environ.get("PUDE_TEST_KEEP_TMP") or tempfile.mkdtemp(prefix="pureunix-pcmanfm-pude-test-")
    print(f"(tmp dir: {tmp})")
    if True:
        serial = os.path.join(tmp, "serial.log")
        qmp = os.path.join(tmp, "qmp.sock")

        qemu = QemuSession(args.iso, serial, qmp, drive_boot=args.drive_boot)
        try:
            print("=== waiting for shell prompt ===")
            qemu.wait_for(r"Enter 'help'", 90)
            time.sleep(1.5)

            print("=== launching pude ===")
            qemu.type_text("pude\n")
            time.sleep(2.0)
            shot("p01-desktop.ppm")

            print("=== opening launcher menu ===")
            qemu.click_at(*menu_btn_center())
            shot("p02-menu-open.ppm")

            print("=== clicking 'File Manager' menu entry ===")
            qemu.click_at(*menu_item_center(PCMANFM_INDEX))
            time.sleep(3.0)
            shot("p03-pcmanfm-just-spawned.ppm")
            time.sleep(3.0)
            shot("p04-pcmanfm-settled.ppm")

            wx, wy = spawn_position(0)
            print("=== clicking inside the PCManFM-Qt window to focus it ===")
            qemu.click_at(wx + 80, wy + 60)
            time.sleep(0.5)
            shot("p05-pcmanfm-focused.ppm")

            print("=== double-clicking a file-list row (browsing check) ===")
            qemu.click_at(wx + 100, wy + 120)
            time.sleep(0.1)
            qemu.click_at(wx + 100, wy + 120)
            time.sleep(1.0)
            shot("p06-pcmanfm-after-double-click.ppm")

            print("=== moving mouse across the window (mouse-move plumbing check) ===")
            qemu.mouse_to(wx + 20, wy + 20)
            qemu.mouse_to(wx + 200, wy + 150)
            shot("p07-pcmanfm-mouse-moved.ppm")

            print("=== emergency whole-desktop quit (Ctrl+F12) back to outer ash ===")
            qemu.send_key_combo("ctrl+f12")
            time.sleep(1.0)
            qemu.type_text("echo pcmanfm_pude_test_wm_exited_ok\n")
            transcript = qemu.wait_for(r"pcmanfm_pude_test_wm_exited_ok", 20)
            if transcript.count("pcmanfm_pude_test_wm_exited_ok") < 1:
                print("FAIL: outer ash shell did not cleanly resume after pude exited")
                sys.exit(1)
            print("PASS: outer shell cleanly restored after pude exited")
        finally:
            qemu.kill()

    print("=== test-pcmanfm-pude complete (visual verification required from screenshots) ===")
    print("Screenshots:", *shots, sep="\n  ")


if __name__ == "__main__":
    main()
