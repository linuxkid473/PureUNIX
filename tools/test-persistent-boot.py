#!/usr/bin/env python3
"""tools/test-persistent-boot.py — proves the persistent disk image
(tools/mkdiskimg.py's output) actually survives a reboot: boots it fresh,
creates a file, kills that QEMU process entirely, boots a brand new QEMU
process against the *same* disk image file, and confirms the file is still
there. A false pass here is essentially impossible — the second boot is a
genuinely separate OS process with no shared memory, so any content it
reads came from bytes actually committed to the image file on disk.

kernel/main.c now auto-logs in as root unconditionally ("Auto-login as
root -- no first-boot wizard, no login prompt. BusyBox ash starts
immediately on every boot.") -- the password-wizard/login-prompt flow this
used to drive no longer exists on any current build (same staleness
tools/vt-inject-test.py's boot_to_shell() was already fixed for -- see
docs/lua-port.md's Testing section). `--password` is accepted but unused,
kept only so existing callers/CLI invocations don't need to change.

Reuses the same QMP send-key driving technique as tools/vt-inject-test.py
(see that file's header comment for why: no usable stdin over `-serial
stdio`, PS/2-only keyboard) but as a standalone script rather than a
vt-inject-test.py script, since "boot twice against the same disk" doesn't
fit that tool's one-fresh-boot-per-script model.

Usage:
    tools/test-persistent-boot.py DISK.img [--password test1234]
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
    ">": ("dot", True), "<": ("comma", True),
}


def char_to_keys(ch):
    if ch.isalpha():
        return [{"type": "qcode", "data": ch.lower()}] if ch.islower() else \
               [{"type": "qcode", "data": "shift"}, {"type": "qcode", "data": ch.lower()}]
    if ch.isdigit():
        return [{"type": "qcode", "data": ch}]
    if ch in BASE_MAP:
        qcode, shift = BASE_MAP[ch]
        keys = [{"type": "qcode", "data": qcode}]
        if shift:
            keys.insert(0, {"type": "qcode", "data": "shift"})
        return keys
    raise ValueError(f"no scancode mapping for character {ch!r}")


class QemuSession:
    def __init__(self, disk_img, serial_log, qmp_sock, usb=False):
        self.serial_log = serial_log
        self.qmp_sock = qmp_sock
        for p in (qmp_sock, serial_log):
            try:
                os.remove(p)
            except FileNotFoundError:
                pass
        if usb:
            # p3=0 forces the emulated drive onto a USB2 port -- qemu-xhci
            # otherwise defaults usb-storage to a USB3/SuperSpeed port this
            # kernel's xHCI driver doesn't enumerate (USB2-only scope; see
            # drivers/xhci.c). Real USB2/USB3 flash drives on real hardware
            # both attach through the same USB2 port-bringup path this
            # exercises -- SuperSpeed enumeration is a separate, distinct
            # gap, not exercised by this test.
            disk_args = [
                "-device", "qemu-xhci,id=xhci,p2=4,p3=0",
                "-drive", f"if=none,id=stick,format=raw,file={disk_img}",
                "-device", "usb-storage,bus=xhci.0,drive=stick",
                "-boot", "order=c",
            ]
        else:
            disk_args = ["-drive", f"file={disk_img},format=raw", "-boot", "c"]
        args = [
            "qemu-system-i386", "-m", "128M",
            *disk_args,
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

    def type_text(self, text, delay=0.02):
        for ch in text:
            self.send_keys(char_to_keys(ch))
            time.sleep(delay)

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
        """Hard, unclean shutdown — no ACPI/quit, just SIGTERM the process,
        the closest QEMU-level proxy to "the machine lost power"/a real
        reboot. Proves persistence survives more than just a graceful path."""
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk_img")
    ap.add_argument("--password", default="test1234")
    ap.add_argument("--usb", action="store_true",
                     help="attach the disk as real USB Mass Storage (qemu-xhci + usb-storage) "
                          "instead of a plain -drive, to exercise drivers/usb_msd.c")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="pureunix-persist-test-") as tmp:
        serial1 = os.path.join(tmp, "serial1.log")
        qmp1 = os.path.join(tmp, "qmp1.sock")

        print("=== boot 1: create file ===")
        qemu = QemuSession(args.disk_img, serial1, qmp1, usb=args.usb)
        try:
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)
            qemu.type_text("echo persistence-worked > /home/test.txt\n")
            time.sleep(0.5)
            qemu.type_text("cat /home/test.txt\n")
            transcript1 = qemu.wait_for(r"persistence-worked", 20)
            if transcript1.count("persistence-worked") < 1:
                print("FAIL: boot 1 never echoed the file back")
                sys.exit(1)
            print("boot 1 OK: /home/test.txt created and readable")
        finally:
            qemu.kill()

        time.sleep(1)

        serial2 = os.path.join(tmp, "serial2.log")
        qmp2 = os.path.join(tmp, "qmp2.sock")

        print("=== boot 2: fresh QEMU process, same disk image ===")
        qemu = QemuSession(args.disk_img, serial2, qmp2, usb=args.usb)
        try:
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)
            qemu.type_text("cat /home/test.txt\n")
            transcript = qemu.wait_for(r"#\s*$", 20)
            if "persistence-worked" not in transcript:
                print("FAIL: /home/test.txt did not survive reboot")
                print(transcript[-2000:])
                sys.exit(1)
            print("boot 2 OK: /home/test.txt survived reboot with correct content")
        finally:
            qemu.kill()

    print("=== PASS: filesystem changes persisted across reboot ===")


if __name__ == "__main__":
    main()
