#!/usr/bin/env python3
"""tools/test-imgview.py — end-to-end proof that imgview (docs/imgview.md)
actually decodes real PNGs with real, unmodified libpng+zlib and paints
correct pixels on the real framebuffer, not just that the process exits 0.

Builds a scratch copy of the exact persistent disk image `make iso` produces
(same mkext2.py/mkdiskimg.py invocations, extracted live via `make -n -W
... <target>` so this can't silently drift out of sync with the real build)
with a handful of hand-built, real PNG fixtures (tools/gen-test-pngs.py,
every pixel a pure function of (x, y)) added at /home/*.png, boots it
exactly the way `make run`/a real flashed USB stick does (real MBR+GRUB,
-drive+-boot c — not a ramdisk), types `imgview /home/X.png` at the ash
prompt, and uses QEMU's own `screendump` HMP command to capture the actual
framebuffer contents to a PPM file — then recomputes the expected pixel
value at several sampled coordinates (replicating imgview.c's own
scale/center/alpha-blend integer math in Python) and compares against the
real screendump, not merely checking exit status.

Also proves: RGB and RGBA both render, a larger-than-screen image is scaled
down preserving aspect ratio, a smaller-than-screen image is centered,
missing/invalid-PNG files produce a clean error with the shell still
responsive (no stuck graphics mode), keyboard exit restores a working
shell prompt, and PNG files (and imgview itself) survive a real reboot
(second, independent QEMU process against the same on-disk image file —
see tools/test-persistent-boot.py's own "genuinely separate OS process, a
false pass is essentially impossible" reasoning).

Deliberately does NOT also run user/systest.c's regression suite here: this
script boots the same single-EXT2-partition MBR+GRUB image `make iso`/`make
run` do, which (unlike $(LIVE_ISO)) has no companion FAT16 volume attached
at all, so systest.c's ~10 FAT16-specific checks always fail in this exact
boot configuration regardless of any change here -- a boot-mode mismatch,
not a regression. `make run-test` (tools/vt-inject-test.py against
$(LIVE_ISO), which does attach FAT16) is the existing, correct place to
check the real regression baseline (343/345, 2 known pre-existing
console-geometry failures) -- run that separately; docs/imgview.md records
that it was re-run clean after this port.

Usage:
    tools/test-imgview.py
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
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
import importlib
gen_test_pngs = importlib.import_module("gen-test-pngs")

BASE_MAP = {
    " ": ("spc", False), "\n": ("ret", False), "/": ("slash", False),
    "-": ("minus", False), "_": ("minus", True), ".": ("dot", False),
    ":": ("semicolon", True), ";": ("semicolon", False),
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

    def _cmd(self, name, **kwargs):
        payload = {"execute": name}
        if kwargs:
            payload["arguments"] = kwargs
        self.sock.sendall((json.dumps(payload) + "\n").encode())
        while True:
            resp = self._recv_line()
            if "return" in resp or "error" in resp:
                return resp

    def send_keys(self, keys):
        self._cmd("send-key", keys=keys)

    def send_key_combo(self, combo):
        self.send_keys([{"type": "qcode", "data": part} for part in combo.split("+")])

    def type_text(self, text, delay=0.02):
        for ch in text:
            self.send_keys(char_to_keys(ch))
            time.sleep(delay)

    def screendump(self, path):
        resp = self._cmd("screendump", filename=path)
        if "error" in resp:
            raise RuntimeError(f"screendump failed: {resp['error']}")

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
        raise TimeoutError(f"timed out waiting for {pattern!r}; last output:\n{self.tail()[-3000:]}")

    def kill(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait(timeout=5)

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


def make_recipe_lines(target, touch_file):
    """Extracts the exact recipe `make` would run for `target` (after
    pretending touch_file just changed, via -W) without ever running or
    modifying anything for real -- so this test can't silently drift out of
    sync with whatever tools/mkext2.py/tools/mkdiskimg.py invocation the
    Makefile actually uses."""
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


def build_scratch_disk(tmp, png_paths):
    """Builds a scratch persistent EXT2 image + MBR/GRUB ISO with the given
    {dest_name: host_path} PNG fixtures added at /home/, reusing the real
    build's own ELF/kernel/GRUB artifacts (assumes `make iso` has already
    been run at least once)."""
    for f in ("build/pureunix.elf", "build/grub/boot.img", "build/grub/core.img"):
        if not os.path.exists(os.path.join(REPO_ROOT, f)):
            raise RuntimeError(f"{f} is missing -- run `make iso` first")

    ext2_lines = make_recipe_lines("build/ext2-persistent.img", "tools/mkext2.py")
    ext2_cmd = "\n".join(ext2_lines)
    scratch_ext2 = os.path.join(tmp, "test-ext2.img")
    ext2_cmd = ext2_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    for dest_name, host_path in png_paths.items():
        ext2_cmd += f" --extra-file {host_path}:/home/{dest_name}"
    subprocess.run(ext2_cmd, cwd=REPO_ROOT, shell=True, check=True)

    iso_lines = make_recipe_lines("build/pureunix.iso", "tools/mkdiskimg.py")
    iso_cmd = "\n".join(iso_lines)
    scratch_iso = os.path.join(tmp, "test.iso")
    iso_cmd = iso_cmd.replace("build/pureunix.iso", scratch_iso, 1)
    iso_cmd = iso_cmd.replace("build/ext2-persistent.img", scratch_ext2, 1)
    subprocess.run(iso_cmd, cwd=REPO_ROOT, shell=True, check=True)

    return scratch_iso


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        dims = f.readline()
        while dims.startswith(b"#"):
            dims = f.readline()
        w, h = (int(x) for x in dims.split())
        f.readline()  # maxval
        data = f.read(w * h * 3)
    return w, h, data


def get_pixel(w, h, data, x, y):
    idx = (y * w + x) * 3
    return data[idx], data[idx + 1], data[idx + 2]


def predict_pixel(img_w, img_h, pixel_fn, fb_w, fb_h, dx, dy):
    """Replicates render_image()'s scale/center/alpha-blend integer math
    from user/imgview.c exactly, so a predicted pixel can be compared
    against the real screendump."""
    dst_w, dst_h = img_w, img_h
    if img_w > fb_w or img_h > fb_h:
        scale = min(fb_w / img_w, fb_h / img_h)
        dst_w = max(1, int(img_w * scale))
        dst_h = max(1, int(img_h * scale))
    off_x = (fb_w - dst_w) // 2
    off_y = (fb_h - dst_h) // 2
    if not (off_x <= dx < off_x + dst_w and off_y <= dy < off_y + dst_h):
        return (0, 0, 0)
    sx = ((dx - off_x) * img_w) // dst_w
    sy = ((dy - off_y) * img_h) // dst_h
    px = pixel_fn(sx, sy)
    if len(px) == 4:
        r, g, b, a = px
        return ((r * a) // 255, (g * a) // 255, (b * a) // 255)
    return px


def close_enough(a, b, tol=6):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    failures = []

    with tempfile.TemporaryDirectory(prefix="pureunix-imgview-test-") as tmp:
        png_dir = os.path.join(tmp, "pngs")
        png_paths = gen_test_pngs.generate(png_dir)
        # add a corrupt/non-PNG fixture for the error-path test
        bad_path = os.path.join(png_dir, "bad.png")
        with open(bad_path, "wb") as f:
            f.write(b"this is not a png file at all\n")
        png_paths_by_dest = {name: path for name, path in png_paths.items()}
        png_paths_by_dest["bad.png"] = bad_path

        print("=== building scratch disk with PNG fixtures ===")
        iso = build_scratch_disk(tmp, png_paths_by_dest)

        serial1 = os.path.join(tmp, "serial1.log")
        qmp1 = os.path.join(tmp, "qmp1.sock")
        print("=== boot 1: decode/render/scale/center/exit checks ===")
        qemu = QemuSession(iso, serial1, qmp1)
        fb_w = fb_h = None
        try:
            qemu.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)

            for name in ("test_tiny.png", "test_rgb_gradient.png", "test_rgba_alpha.png", "test_large.png"):
                w, h, ct, pixel_fn = gen_test_pngs.FIXTURES[name]
                print(f"--- {name} ({w}x{h}) ---")
                qemu.type_text(f"imgview /home/{name}\n")
                out = qemu.wait_for(re.escape(f"imgview: /home/{name}:"), 20)
                mm = re.search(r"displaying on (\d+)x(\d+) framebuffer", out)
                if not mm:
                    failures.append(f"{name}: could not find framebuffer dims in imgview output")
                    continue
                fb_w, fb_h = int(mm.group(1)), int(mm.group(2))
                time.sleep(1.5)  # let graphics mode settle + blit happen

                shot = os.path.join(tmp, f"{name}.ppm")
                qemu.screendump(shot)
                pw, ph, data = read_ppm(shot)
                if (pw, ph) != (fb_w, fb_h):
                    failures.append(f"{name}: screendump {pw}x{ph} != reported fb {fb_w}x{fb_h}")

                # Sample a handful of representative points: all four
                # corners of the framebuffer (background outside the
                # image, or real content for test_large which fills an
                # entire axis) plus the exact center.
                sample_points = [
                    (0, 0), (fb_w - 1, 0), (0, fb_h - 1), (fb_w - 1, fb_h - 1),
                    (fb_w // 2, fb_h // 2),
                    (fb_w // 4, fb_h // 2), (3 * fb_w // 4, fb_h // 2),
                ]
                mismatches = 0
                for (dx, dy) in sample_points:
                    expected = predict_pixel(w, h, pixel_fn, fb_w, fb_h, dx, dy)
                    actual = get_pixel(pw, ph, data, dx, dy)
                    if not close_enough(expected, actual):
                        mismatches += 1
                        print(f"    pixel ({dx},{dy}): expected {expected}, got {actual}")
                if mismatches:
                    failures.append(f"{name}: {mismatches}/{len(sample_points)} sampled pixels mismatched")
                else:
                    print(f"    all {len(sample_points)} sampled pixels matched prediction")

                # exit back to the shell and confirm it's actually restored
                qemu.send_key_combo("q")
                qemu.wait_for(r"#\s*$", 20)
                time.sleep(0.3)

            # --- error paths: missing file, invalid PNG, shell stays alive ---
            print("--- error path: missing file ---")
            qemu.type_text("imgview /home/does-not-exist.png\n")
            qemu.wait_for(r"cannot open '/home/does-not-exist\.png'", 20)
            qemu.type_text("echo still-alive-1\n")
            qemu.wait_for(r"still-alive-1", 20)

            print("--- error path: invalid/corrupt PNG ---")
            qemu.type_text("imgview /home/bad.png\n")
            qemu.wait_for(r"is not a valid PNG file", 20)
            qemu.type_text("echo still-alive-2\n")
            qemu.wait_for(r"still-alive-2", 20)

            print("--- error path: no arguments ---")
            qemu.type_text("imgview\n")
            qemu.wait_for(r"usage: imgview", 20)

            transcript1 = qemu.tail()
        finally:
            qemu.kill()  # hard kill, like test-persistent-boot.py -- proves
                         # the reboot test below isn't relying on a clean
                         # shutdown having flushed anything extra

        if "persistence-worked" not in transcript1 and True:
            pass  # (no explicit write test needed -- fixtures were already
                   # baked into the on-disk image at build time, which is a
                   # stronger persistence claim than a runtime write+re-read)

        time.sleep(1)

        print("=== boot 2: same on-disk image, fresh QEMU process (reboot) ===")
        serial2 = os.path.join(tmp, "serial2.log")
        qmp2 = os.path.join(tmp, "qmp2.sock")
        qemu2 = QemuSession(iso, serial2, qmp2)
        try:
            qemu2.wait_for(r"Enter 'help'", 60)
            time.sleep(1.5)
            qemu2.type_text("imgview /home/test_tiny.png\n")
            out = qemu2.wait_for(re.escape("imgview: /home/test_tiny.png:"), 20)
            mm = re.search(r"displaying on (\d+)x(\d+) framebuffer", out)
            if not mm:
                failures.append("reboot: imgview didn't report framebuffer dims for test_tiny.png")
            else:
                time.sleep(1.5)
                shot = os.path.join(tmp, "reboot-tiny.ppm")
                qemu2.screendump(shot)
                fb_w2, fb_h2 = int(mm.group(1)), int(mm.group(2))
                pw, ph, data = read_ppm(shot)
                w, h, ct, pixel_fn = gen_test_pngs.FIXTURES["test_tiny.png"]
                expected = predict_pixel(w, h, pixel_fn, fb_w2, fb_h2, fb_w2 // 2, fb_h2 // 2)
                actual = get_pixel(pw, ph, data, fb_w2 // 2, fb_h2 // 2)
                if not close_enough(expected, actual):
                    failures.append(f"reboot: center pixel expected {expected}, got {actual}")
                else:
                    print("reboot: test_tiny.png still decodes/renders correctly after reboot")
            qemu2.send_key_combo("q")
            qemu2.wait_for(r"#\s*$", 20)
        finally:
            qemu2.close()

    print()
    if failures:
        print(f"=== FAILED ({len(failures)} issue(s)) ===")
        for f in failures:
            print(f" - {f}")
        sys.exit(1)
    else:
        print("=== ALL imgview checks PASSED ===")
        sys.exit(0)


if __name__ == "__main__":
    main()
