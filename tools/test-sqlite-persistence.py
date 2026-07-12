#!/usr/bin/env python3
"""tools/test-sqlite-persistence.py — proves the SQLite port
(docs/sqlite-port.md) is genuinely usable and persistent, not just
"compiles and launches": boots the real persistent disk image
(tools/mkdiskimg.py's output), runs `sqlite3 /home/test.db` from BusyBox
ash with no PATH setup, creates a table, inserts/updates/deletes rows
inside an explicit transaction, creates an index, verifies the resulting
data with a SELECT, exits and reopens the same database (still within the
same boot — proves close()-time flush + a fresh open() round-trip through
PureUNIX's whole-file-in-memory VFS), then hard-kills that QEMU process
entirely and boots a brand new QEMU process against the *same* disk image
file to confirm the exact data survived a real reboot.

Same QMP send-key driving technique as tools/test-persistent-boot.py (see
that file's header comment for why: no usable stdin over `-serial stdio`,
PS/2-only keyboard); this is a standalone script rather than a
vt-inject-test.py script for the same "boot twice against the same disk"
reason test-persistent-boot.py already is one.

Usage:
    tools/test-sqlite-persistence.py DISK.img
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
    ",": ("comma", False), ":": ("semicolon", True), ";": ("semicolon", False),
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

    def type_text(self, text, delay=0.02):
        for ch in text:
            self.send_keys(char_to_keys(ch))
            time.sleep(delay)

    def type_line(self, text, delay=0.02):
        self.type_text(text, delay)
        self.type_text("\n", delay)

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
        """Hard, unclean shutdown — SIGTERM the QEMU process itself, the
        closest proxy to "the machine lost power"/a real reboot. Proves
        persistence survives more than just a graceful path."""
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)


def boot_to_shell(qemu):
    # kernel/main.c auto-logs in as root unconditionally -- no wizard, no
    # login prompt, BusyBox ash starts immediately on every boot. See
    # tools/test-persistent-boot.py's own header comment for the same fix.
    qemu.wait_for(r"Enter 'help'", 60)
    time.sleep(1.5)


# Expected final table contents after boot 1's edits: alice's row updated
# (val 10 -> 99), bob's row deleted, carol inserted, dave inserted inside
# an explicit transaction -- rowids assigned in insertion order (1=alice,
# 2=bob [deleted], 3=carol, 4=dave; SQLite never reuses a rowid within one
# INTEGER PRIMARY KEY sequence, so dave lands on 4, not 3, even though
# bob's row 2 was deleted first).
EXPECTED_ROWS = ["1|alice|99", "3|carol|30", "4|dave|40"]
SELECT_SQL = "SELECT id,name,val FROM t ORDER BY id;"


def build_and_verify(qemu):
    """Boot 1: create the schema/data through a real interactive REPL
    session (proves the `sqlite3 /home/test.db` command line from the task
    works with no PATH setup), covering every DML/DDL category the task
    calls out: table creation, inserts, an update, a delete, an index, and
    an explicit transaction."""
    qemu.type_line("sqlite3 /home/test.db")
    qemu.wait_for(r"SQLite version", 15)
    qemu.wait_for(r"sqlite>", 10)

    # Default shell output is a unicode box-drawing table (great
    # interactively, painful to regex out of a serial log) -- .mode list
    # switches to the classic "|"-delimited rows this test parses.
    qemu.type_line(".mode list")
    time.sleep(0.2)

    for stmt in [
        "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, val INTEGER);",
        "INSERT INTO t(name,val) VALUES('alice',10);",
        "INSERT INTO t(name,val) VALUES('bob',20);",
        "INSERT INTO t(name,val) VALUES('carol',30);",
        "UPDATE t SET val=99 WHERE name='alice';",
        "DELETE FROM t WHERE name='bob';",
        "CREATE INDEX idx_t_name ON t(name);",
        "BEGIN;",
        "INSERT INTO t(name,val) VALUES('dave',40);",
        "COMMIT;",
    ]:
        qemu.type_line(stmt)
        time.sleep(0.3)

    qemu.type_line(SELECT_SQL)
    transcript = qemu.wait_for(re.escape(EXPECTED_ROWS[-1]), 15)
    for row in EXPECTED_ROWS:
        if row not in transcript:
            print(f"FAIL: expected row {row!r} missing from in-session SELECT output")
            print(transcript[-2000:])
            sys.exit(1)
    print("boot 1 OK: schema/inserts/update/delete/index/transaction all applied correctly")

    qemu.type_line(".exit")
    qemu.wait_for(r"#\s*$", 15)


def verify_reopen(qemu, label):
    """A one-shot, non-interactive `sqlite3 DBFILE "SQL"` invocation (the
    CLI's own standard non-REPL mode) -- simpler to assert on than another
    REPL transcript, and exercises a completely fresh open() of the
    on-disk file rather than reusing any in-process state."""
    qemu.type_line(f'sqlite3 -list /home/test.db "{SELECT_SQL}"')
    transcript = qemu.wait_for(re.escape(EXPECTED_ROWS[-1]), 15)
    for row in EXPECTED_ROWS:
        if row not in transcript:
            print(f"FAIL ({label}): expected row {row!r} missing")
            print(transcript[-2000:])
            sys.exit(1)
    print(f"{label} OK: all rows present with correct data")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk_img")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="pureunix-sqlite-test-") as tmp:
        serial1 = os.path.join(tmp, "serial1.log")
        qmp1 = os.path.join(tmp, "qmp1.sock")

        print("=== boot 1: create database, verify in-session and after exit+reopen ===")
        qemu = QemuSession(args.disk_img, serial1, qmp1)
        try:
            boot_to_shell(qemu)
            build_and_verify(qemu)
            verify_reopen(qemu, "boot 1 reopen (same boot, fresh open())")
        finally:
            qemu.kill()

        time.sleep(1)

        serial2 = os.path.join(tmp, "serial2.log")
        qmp2 = os.path.join(tmp, "qmp2.sock")

        print("=== boot 2: fresh QEMU process, same disk image ===")
        qemu = QemuSession(args.disk_img, serial2, qmp2)
        try:
            boot_to_shell(qemu)
            verify_reopen(qemu, "boot 2 (real reboot)")
        finally:
            qemu.kill()

    print("=== PASS: SQLite database created, modified, and correctly persisted across reboot ===")


if __name__ == "__main__":
    main()
