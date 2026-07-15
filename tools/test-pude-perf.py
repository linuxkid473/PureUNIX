#!/usr/bin/env python3
"""tools/test-pude-perf.py — region-dependent cursor-lag investigation for
pude (docs/pude.md). Boots the real persistent disk image built by
`make iso`, launches `pude` from a real BusyBox ash prompt, opens a real
PUTerm window, then drives real HMP mouse motion (same technique as
tools/test-pude.py -- see that file's module docstring for why QMP's own
`input-send-event` can't be used) confined to each of six regions in turn:

  1. desktop background hover
  2. PUTerm's client-area hover
  3. PUTerm's title-bar hover
  4. PUTerm's close-button hover (never actually clicked)
  5. active-window dragging (title-bar drag out and back)
  6. active-window resizing (resize-grip drag out and back)

Before/after each phase, Ctrl+F11 tells pude to append one delta snapshot
(user/pude.c's pude_perf_dump()) to /pude_perf.log -- events dequeued,
compositor passes, windows recomposited, app render() calls, glyphs/pixels
drawn, and elapsed wall time since the previous dump. Snapshots are
unlabeled (a single hotkey can't carry a phase name) and rely on this
script's own strict phase ordering to attribute each one.

After all six phases, PUTerm's own shell is used to `cat /pude_perf.log`
(pude itself owns the whole screen in graphics mode, so there is no other
way to read a file it wrote mid-session -- see docs/pude.md), and the
result is parsed and printed as a per-phase comparison table.

Usage:
    tools/test-pude-perf.py DISK.img
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

# ---- QEMU/QMP session plumbing -- identical technique to tools/test-pude.py
# (see that file's docstring for why HMP mouse_move/mouse_button, not QMP
# input-send-event, is the only thing that actually moves this environment's
# real PS/2 mouse). Duplicated rather than imported so this script has no
# import-path dependency on the other test's module layout.

BASE_MAP = {
    " ": ("spc", False), "\n": ("ret", False), "/": ("slash", False),
    "-": ("minus", False), "_": ("minus", True), ".": ("dot", False),
    ":": ("semicolon", True), ";": ("semicolon", False),
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

    def walk_rel(self, dx, dy, steps=20, delay=0.02):
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

    def mouse_to(self, x, y, settle=0.1):
        self.walk_rel(-9999, -9999, steps=30, delay=0.015)
        time.sleep(0.15)
        self.walk_rel(x, y, steps=max(4, (abs(x) + abs(y)) // 40), delay=0.02)
        time.sleep(settle)

    def click_at(self, x, y, settle=0.15):
        self.mouse_to(x, y, settle)
        self.mouse_button(True)
        time.sleep(0.08)
        self.mouse_button(False)
        time.sleep(settle)

    def drag_from(self, from_x, from_y, dx, dy, steps=15, delay=0.03):
        self.mouse_to(from_x, from_y)
        self.mouse_button(True)
        time.sleep(0.06)
        self.walk_rel(dx, dy, steps=steps, delay=delay)
        time.sleep(0.08)
        self.mouse_button(False)
        time.sleep(0.15)

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


# ---- pude chrome geometry (user/pude.c) -- kept in sync by hand, same
# caveat as tools/test-pude.py: no shared source of truth with the C WM.
BORDER = 3
TITLEBAR_PAD = 6
FONT_CELL_H = 17
TITLEBAR_H = FONT_CELL_H + TITLEBAR_PAD * 2  # 29
RESIZE_GRIP = 22
CLOSE_BTN_W = 20
MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H = 8, 8, 90, 26
MENU_ITEM_H = 28
MARGIN = 24

PUTERM_CLIENT_W, PUTERM_CLIENT_H = 80 * 8, 24 * FONT_CELL_H  # 640 x 408

# First window spawned (cascade index 0) -- see user/pude.c's spawn_window().
WIN_X, WIN_Y = MARGIN, MARGIN
WIN_W = PUTERM_CLIENT_W + 2 * BORDER            # 646
WIN_H = PUTERM_CLIENT_H + 2 * BORDER + TITLEBAR_H  # 443


def menu_button_center():
    return MENU_BTN_X + MENU_BTN_W // 2, MENU_BTN_Y + MENU_BTN_H // 2


def launcher_item_center(index):
    py = MENU_BTN_Y + MENU_BTN_H + 4
    iy = py + 4 + index * MENU_ITEM_H
    return MENU_BTN_X + 4 + 80, iy + (MENU_ITEM_H - 2) // 2


def close_button_center():
    bh = TITLEBAR_H - 2 * (TITLEBAR_PAD // 2 + 1)
    bx = WIN_X + WIN_W - BORDER - TITLEBAR_PAD - CLOSE_BTN_W
    by = WIN_Y + BORDER + (TITLEBAR_H - bh) // 2
    return bx + CLOSE_BTN_W // 2, by + bh // 2


def resize_grip_center():
    return WIN_X + WIN_W - RESIZE_GRIP // 2, WIN_Y + WIN_H - RESIZE_GRIP // 2


PHASES = [
    "desktop", "client_area", "title_bar", "close_button", "drag_move", "resize",
]

FIELDS = [
    "seq", "ms", "iter", "ev", "mot", "btn", "key", "frames",
    "winrecomp", "apprender", "fullredraws", "cheappasses", "damagerects", "damagearea",
    "glyphs", "putpix", "fillrect", "fillpix",
]


# kernel/vt.c's own (pre-existing, unrelated) framebuffer-console scroll
# perf print shares this serial UART and is not synchronized with our own
# writes -- it can physically interleave mid-line inside a PUDEPERF record
# (confirmed empirically: "...fillrect=610\nPERF scroll rows=... glyph_cyc=N\n
# fillpix=...". Stripped out before parsing, then key=value pairs are
# pulled out with a regex scan (robust to the stray newlines this leaves
# behind) rather than a naive per-line split.
_NOISE_RE = re.compile(r"PERF scroll rows=\d+ bytes=\d+ cyc=\d+ glyphs=\d+ glyph_cyc=\d+")
_KV_RE = re.compile(r"(\w+)=(\d+)")


def parse_perf_log(text):
    text = _NOISE_RE.sub(" ", text)
    rows = []
    for chunk in text.split("PUDEPERF"):
        chunk = chunk.strip()
        if not chunk:
            continue
        fields = {k: int(v) for k, v in _KV_RE.findall(chunk) if k in FIELDS}
        if "seq" in fields:
            rows.append(fields)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk_img")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="pureunix-pude-perf-") as tmp:
        serial = os.path.join(tmp, "serial.log")
        qmp = os.path.join(tmp, "qmp.sock")

        qemu = QemuSession(args.disk_img, serial, qmp)
        try:
            print("=== waiting for shell prompt ===")
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            print("=== clearing any stale /pude_perf.log from a prior run ===")
            qemu.type_text("rm -f /pude_perf.log\n")
            time.sleep(0.3)

            print("=== launching pude ===")
            qemu.type_text("pude\n")
            time.sleep(2.0)

            print("=== opening PUTerm from the launcher ===")
            qemu.click_at(*menu_button_center())
            qemu.click_at(*launcher_item_center(0))
            time.sleep(1.0)

            print("=== discarding startup noise (baseline dump) ===")
            qemu.send_key_combo("ctrl+f11")
            time.sleep(0.3)

            def dump():
                qemu.send_key_combo("ctrl+f11")
                time.sleep(0.3)

            # 1. desktop background -- clear of the 646x443 PUTerm window
            # (occupies x:[24,670) y:[24,467)) and the Menu button.
            print("=== phase: desktop hover ===")
            qemu.mouse_to(1000, 600)
            for _ in range(6):
                qemu.walk_rel(80, 0, steps=10, delay=0.02)
                qemu.walk_rel(-80, 0, steps=10, delay=0.02)
            dump()

            # 2. client area
            print("=== phase: client-area hover ===")
            cx, cy = WIN_X + BORDER + 100, WIN_Y + BORDER + TITLEBAR_H + 100
            qemu.mouse_to(cx, cy)
            for _ in range(6):
                qemu.walk_rel(150, 100, steps=10, delay=0.02)
                qemu.walk_rel(-150, -100, steps=10, delay=0.02)
            dump()

            # 3. title bar -- well clear of the close button
            print("=== phase: title-bar hover ===")
            tx, ty = WIN_X + 100, WIN_Y + BORDER + TITLEBAR_H // 2
            qemu.mouse_to(tx, ty)
            for _ in range(6):
                qemu.walk_rel(150, 0, steps=10, delay=0.02)
                qemu.walk_rel(-150, 0, steps=10, delay=0.02)
            dump()

            # 4. close button -- hover only, mouse_button is never pressed
            print("=== phase: close-button hover ===")
            bx, by = close_button_center()
            qemu.mouse_to(bx, by)
            for _ in range(6):
                qemu.walk_rel(8, 4, steps=6, delay=0.02)
                qemu.walk_rel(-8, -4, steps=6, delay=0.02)
            dump()

            # 5. drag the window by its title bar, then back (net position
            # unchanged so later geometry -- the resize grip -- stays valid)
            print("=== phase: drag-move ===")
            qemu.drag_from(WIN_X + 100, WIN_Y + BORDER + TITLEBAR_H // 2, 150, 100)
            qemu.drag_from(WIN_X + 100 + 150, WIN_Y + BORDER + TITLEBAR_H // 2 + 100, -150, -100)
            dump()

            # 6. resize via the grip, then back
            print("=== phase: resize ===")
            gx, gy = resize_grip_center()
            qemu.drag_from(gx, gy, 80, 60)
            qemu.drag_from(gx + 80, gy + 60, -80, -60)
            dump()

            # PUTerm's own shell runs under a real pty that pude renders
            # into its window -- its stdout never reaches this VT's
            # console/serial device, only pude's own framebuffer drawing
            # does (see docs/pude.md). So /pude_perf.log can only be read
            # back through the *outer* ash, after pude itself exits.
            print("=== emergency whole-desktop quit ===")
            qemu.send_key_combo("ctrl+f12")
            # Leaving graphics mode makes kernel/vt.c repaint this VT's
            # whole restored text console in one burst -- give that (and
            # its own scroll-perf debug prints, which share this serial
            # UART and can otherwise interleave mid-line with cat's output
            # right below) time to fully settle before reading anything
            # back, or the earliest lines of the log get corrupted.
            time.sleep(3.0)
            qemu.type_text("echo settled_ok\n")
            qemu.wait_for(r"settled_ok", 20)
            time.sleep(0.5)

            # drivers/vga.c's own (pre-existing, unrelated) scroll-perf
            # instrumentation fires synchronously, straight to this same
            # serial port, whenever the *active* console scrolls -- and
            # with a whole boot+session's worth of history already on
            # screen, cat's own output scrolls it almost immediately,
            # splicing "PERF scroll ..." text mid-line into our own
            # PUDEPERF output. Clearing the screen first resets the
            # console to row 0 so the (short) dump fits without scrolling
            # at all.
            print("=== clearing the console before reading back /pude_perf.log ===")
            qemu.type_text("clear\n")
            time.sleep(0.3)

            print("=== reading back /pude_perf.log from the outer ash ===")
            qemu.type_text("cat /pude_perf.log\n")
            time.sleep(1.0)
            qemu.type_text("echo wm_exited_ok\n")
            transcript = qemu.wait_for(r"wm_exited_ok", 20)
        finally:
            qemu.kill()

    rows = parse_perf_log(transcript)
    # seq=1 is the "discard startup noise" baseline dump, not one of the six
    # measured phases -- see the ctrl+f11 sent right after PUTerm opens.
    # Matched by seq number (not list position) since the noisy serial
    # readback can occasionally drop or duplicate a row.
    labels = ["(baseline, discarded)"] + PHASES
    by_seq = {}
    for row in rows:
        by_seq.setdefault(row["seq"], []).append(row)

    print(f"\n=== parsed {len(rows)} PUDEPERF rows across {len(by_seq)} distinct seq numbers "
          f"(expect {len(labels)}) ===")

    print(f"\n{'phase':<24}" + "".join(f"{f:>11}" for f in FIELDS))
    ok = True
    for seq in range(1, len(labels) + 1):
        label = labels[seq - 1]
        matches = by_seq.get(seq, [])
        if len(matches) != 1 or any(f not in matches[0] for f in FIELDS):
            # seq=1 is the discarded baseline -- its data is never used, so
            # a noisy-serial-mangled row there doesn't invalidate the run.
            if seq != 1:
                ok = False
            print(f"{label:<24}  MISSING/INCOMPLETE ({len(matches)} row(s) with seq={seq})")
            continue
        row = matches[0]
        print(f"{label:<24}" + "".join(f"{row.get(f, -1):>11}" for f in FIELDS))

    extra_seqs = sorted(s for s in by_seq if s < 1 or s > len(labels))
    if extra_seqs:
        ok = False
        print(f"\nUnexpected extra seq number(s): {extra_seqs}")

    if not ok:
        print("\nFAIL: did not get a complete PUDEPERF row for every measured phase")
        sys.exit(1)

    print("\nPASS: all phases dumped a snapshot")


if __name__ == "__main__":
    main()
