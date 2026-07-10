#!/usr/bin/env python3
"""tools/vt-inject-test.py — headless QEMU test driver for PureUnix.

PureUnix's keyboard driver is PS/2-only (drivers/keyboard.c): there is no
usable stdin over `-serial stdio`, so exercising anything that needs real
keystrokes (login, a shell prompt, Ctrl+C/Ctrl+Z/Alt+Fn) requires actually
injecting PS/2 scancodes. This drives QEMU headless via its QMP socket
(`send-key`) and reads output back from a serial log file, scripted by a
small line-oriented instruction file:

    TYPE <text>          types <text> then Enter
    KEY <qcode>[+<qcode>...]   one simultaneous keypress, e.g. "ctrl+c",
                                "shift+z", or a bare "ret"/"alt+f2"
    WAIT <regex>          blocks until <regex> matches new serial output
                          (Python re, searched against the full log)
    SLEEP <seconds>       fixed pause (float)

Blank lines and lines starting with # are ignored. See docs/
process-management.md for the job-control scenarios this exists for
(Ctrl+C/Ctrl+Z reaching only the active VT's foreground process group,
switching VTs never interrupting a background job on another VT) and
user/systest.c for everything that doesn't need real keystrokes at all
(that's exercised by just running systest.elf from a TYPE line and
checking this script's own summary line, not by anything fancier).

Usage:
    tools/vt-inject-test.py SCRIPT [SCRIPT ...]
    tools/vt-inject-test.py --iso build/pureunix.iso --password test1234 script.txt

Every SCRIPT boots a *fresh* QEMU instance (first-boot password wizard +
login handled automatically before your script's own instructions run).
Exits nonzero if any script errors out (a WAIT that times out) or if the
serial log ever shows "FAIL: " with a nonzero count from user/systest.c's
own summary format.
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

DEFAULT_ISO = "build/pureunix.iso"
DEFAULT_PASSWORD = "test1234"

# char -> (qcode, needs_shift). Extend as new scripts need more symbols.
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
    def __init__(self, iso, serial_log, qmp_sock, extra_args=None):
        self.serial_log = serial_log
        self.qmp_sock = qmp_sock
        for p in (qmp_sock, serial_log):
            try:
                os.remove(p)
            except FileNotFoundError:
                pass
        args = [
            "qemu-system-i386", "-m", "128M", "-cdrom", iso, "-boot", "d",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-serial", f"file:{serial_log}",
            "-qmp", f"unix:{qmp_sock},server,nowait",
            "-no-reboot", "-no-shutdown", "-display", "none",
        ] + (extra_args or [])
        self.proc = subprocess.Popen(args)
        self.sock = self._connect()
        self.buf = [b""]
        self._recv_line()  # QMP greeting
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

    def send_key_combo(self, combo):
        keys = [{"type": "qcode", "data": part} for part in combo.split("+")]
        self.send_keys(keys)

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

    def close(self):
        try:
            self._cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def boot_to_shell(qemu, password):
    qemu.wait_for(r"Set a password for the 'root' account", 60)
    time.sleep(0.5)
    qemu.type_text(password + "\n")
    time.sleep(0.5)
    qemu.type_text(password + "\n")
    qemu.wait_for(r"login:", 30)
    time.sleep(0.3)
    qemu.type_text("root\n")
    time.sleep(0.3)
    qemu.wait_for(r"Password:", 15)
    time.sleep(0.3)
    qemu.type_text(password + "\n")
    time.sleep(1.5)


def run_script(qemu, lines):
    for lineno, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cmd, _, rest = line.partition(" ")
        rest = rest.strip()
        try:
            if cmd == "TYPE":
                qemu.type_text(rest)
                qemu.type_text("\n")
            elif cmd == "KEY":
                qemu.send_key_combo(rest)
            elif cmd == "WAIT":
                qemu.wait_for(rest)
            elif cmd == "SLEEP":
                time.sleep(float(rest))
            else:
                raise ValueError(f"unknown instruction {cmd!r}")
        except Exception as e:
            raise RuntimeError(f"{lineno}: {line!r} failed: {e}") from e


def check_systest_summary(transcript):
    m = re.search(r"Tests:\s*(\d+).*?PASS:\s*(\d+).*?FAIL:\s*(\d+)", transcript, re.S)
    if not m:
        return None
    total, passed, failed = (int(x) for x in m.groups())
    return total, passed, failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scripts", nargs="+", help="script file(s) to run, one fresh boot each")
    ap.add_argument("--iso", default=DEFAULT_ISO)
    ap.add_argument("--password", default=DEFAULT_PASSWORD)
    ap.add_argument("--extra-qemu-arg", action="append", default=[], dest="extra_args")
    args = ap.parse_args()

    overall_ok = True
    for script_path in args.scripts:
        with open(script_path) as f:
            lines = f.readlines()

        script_ok = True
        with tempfile.TemporaryDirectory(prefix="pureunix-vt-test-") as tmp:
            serial_log = os.path.join(tmp, "serial.log")
            qmp_sock = os.path.join(tmp, "qmp.sock")
            print(f"=== {script_path} ===")
            qemu = QemuSession(args.iso, serial_log, qmp_sock, args.extra_args)
            try:
                boot_to_shell(qemu, args.password)
                run_script(qemu, lines)
                time.sleep(1)
                transcript = qemu.tail()
            except Exception as e:
                print(f"FAIL: {script_path}: {e}")
                overall_ok = False
                continue
            finally:
                qemu.close()

            summary = check_systest_summary(transcript)
            if summary:
                total, passed, failed = summary
                print(f"systest: {passed}/{total} passed, {failed} failed")
                if failed > 0:
                    script_ok = False
                    overall_ok = False
            print(f"=== {script_path}: {'OK' if script_ok else 'FAILED'} ===")

    sys.exit(0 if overall_ok else 1)


if __name__ == "__main__":
    main()
