#!/usr/bin/env python3
"""tools/test-ncurses-demo.py — interactive QEMU test driver for the real
ncurses port (docs/ncurses-port.md): boots the persistent disk image built
by `make iso`, drives /bin/ncdemo with real injected keystrokes (arrows,
Enter, q) via the same QMP send-key technique test-persistent-boot.py and
vt-inject-test.py already use (PureUnix's keyboard is PS/2-only — there is
no usable stdin over `-serial stdio`), and captures QMP `screendump`
screenshots at key points so the actual rendered framebuffer content can be
inspected visually, since a full-screen ncurses redraw doesn't produce a
line-oriented transcript the way plain shell output does.

Also drives an Alt+F2 / Alt+F1 VT switch away from and back to a running
ncdemo, to verify vga.c's per-VT repaint-from-buffer path (kernel/vt.c's
vt_switch()) doesn't corrupt an ncurses screen or break its input queue —
screenshotted before the switch and after returning, for a byte-identical
comparison.

Usage:
    tools/test-ncurses-demo.py DISK.img --screenshot-dir DIR
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
        # Give QEMU a moment to finish flushing the PPM to disk.
        time.sleep(0.3)

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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk_img")
    ap.add_argument("--screenshot-dir", required=True)
    args = ap.parse_args()

    os.makedirs(args.screenshot_dir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pureunix-ncurses-test-") as tmp:
        serial = os.path.join(tmp, "serial.log")
        qmp = os.path.join(tmp, "qmp.sock")

        qemu = QemuSession(args.disk_img, serial, qmp)
        try:
            print("=== waiting for shell prompt ===")
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            print("=== launching ncdemo ===")
            qemu.type_text("ncdemo\n")
            time.sleep(1.0)
            shot = os.path.join(args.screenshot_dir, "01-menu.ppm")
            qemu.screendump(shot)
            print(f"screenshot: {shot}")

            print("=== navigating to Colors demo (already selected) + Enter ===")
            qemu.send_key_combo("ret")
            time.sleep(0.8)
            shot = os.path.join(args.screenshot_dir, "02-colors.ppm")
            qemu.screendump(shot)
            print(f"screenshot: {shot}")
            qemu.send_key_combo("spc")  # "press any key" — any key includes space
            time.sleep(0.5)

            print("=== Down to Cursor keys demo + Enter ===")
            qemu.send_key_combo("down")
            time.sleep(0.3)
            qemu.send_key_combo("ret")
            time.sleep(0.8)
            shot = os.path.join(args.screenshot_dir, "03-keys-before.ppm")
            qemu.screendump(shot)
            print(f"screenshot: {shot}")

            print("=== driving arrow keys inside the keys demo ===")
            for combo in ["down", "down", "right", "right", "right", "up"]:
                qemu.send_key_combo(combo)
                time.sleep(0.15)
            shot = os.path.join(args.screenshot_dir, "04-keys-after.ppm")
            qemu.screendump(shot)
            print(f"screenshot: {shot}")
            qemu.send_key_combo("q")
            time.sleep(0.5)

            print("=== VT-switch-away/back safety: Alt+F2 then Alt+F1 ===")
            shot_before = os.path.join(args.screenshot_dir, "05-before-vtswitch.ppm")
            qemu.screendump(shot_before)
            qemu.send_key_combo("alt+f2")
            time.sleep(1.0)
            shot_vt2 = os.path.join(args.screenshot_dir, "06-vt2.ppm")
            qemu.screendump(shot_vt2)
            print(f"screenshot: {shot_vt2}")
            qemu.send_key_combo("alt+f1")
            time.sleep(1.0)
            shot_after = os.path.join(args.screenshot_dir, "07-after-vtswitch.ppm")
            qemu.screendump(shot_after)
            print(f"screenshot: {shot_after}")
            if open(shot_before, "rb").read() != open(shot_after, "rb").read():
                print("FAIL: framebuffer content changed across a VT switch away and back")
                sys.exit(1)
            print("VT switch OK: ncdemo's screen is byte-identical before/after")

            print("=== quitting ncdemo (q) and verifying the shell is restored ===")
            qemu.send_key_combo("q")
            time.sleep(0.8)
            qemu.type_text("echo shell-restored-ok\n")
            transcript = qemu.wait_for(r"shell-restored-ok", 20)
            if transcript.count("shell-restored-ok") < 1:
                print("FAIL: shell did not cleanly resume after ncdemo exited")
                sys.exit(1)
            print("PASS: shell cleanly restored after ncdemo exited")
        finally:
            qemu.kill()

    print("=== PASS: ncurses demo interactive test complete ===")


if __name__ == "__main__":
    main()
