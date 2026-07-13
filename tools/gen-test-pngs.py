#!/usr/bin/env python3
"""tools/gen-test-pngs.py — hand-builds a handful of small, real, valid PNG
files (no PIL/ImageMagick dependency: only the stdlib zlib module, the same
DEFLATE codec imgview itself will decompress with) used as fixtures by
tools/test-imgview.py.

Each one exercises a different part of the imgview/libpng/zlib pipeline:
  - test_tiny.png:         16x16 solid RGB, smaller than any real
                           framebuffer -- centering with no scaling.
  - test_rgb_gradient.png: 64x64 opaque RGB gradient -- another
                           no-scaling/centering case, with per-pixel color
                           content that's cheap to predict exactly.
  - test_rgba_alpha.png:   100x100 RGBA with a genuine alpha ramp over a
                           checkerboard -- exercises the RGBA/alpha-blend
                           path (color_type 6), not just opaque RGB.
  - test_large.png:        2000x1200 RGB, larger than any real framebuffer
                           on either axis -- forces the nearest-neighbour
                           downscale-preserving-aspect-ratio path.

Every pixel's content is a pure function of (x, y) so tools/test-imgview.py
can recompute the exact expected value at any sampled coordinate and compare
it against a real QEMU framebuffer screendump, rather than merely checking
that imgview exits 0.
"""
import os
import struct
import sys
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def write_png(path: str, width: int, height: int, color_type: int, pixel_fn):
    """pixel_fn(x, y) -> bytes tuple: 3 bytes for color_type 2 (RGB), 4 for
    color_type 6 (RGBA). Encodes one uncompressed IDAT with filter type 0
    (None) on every scanline -- the simplest valid, real PNG encoding, not a
    shortcut that skips zlib: zlib.compress() still does real DEFLATE."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # per-scanline filter type: None
        for x in range(width):
            raw.extend(pixel_fn(x, y))
    compressed = zlib.compress(bytes(raw), 9)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", compressed))
        f.write(chunk(b"IEND", b""))


def rgb_gradient(x, y):
    return bytes((x * 4 % 256, y * 4 % 256, (x + y) * 2 % 256))


def rgba_alpha(x, y):
    checker = 255 if ((x // 8) + (y // 8)) % 2 == 0 else 0
    a = int(255 * (x / 99))
    return bytes((checker, 255 - checker, 128, a))


def large_gradient(x, y):
    return bytes((x % 256, y % 256, (x ^ y) % 256))


def small_solid(x, y):
    return bytes((0, 180, 220))


FIXTURES = {
    "test_tiny.png": (16, 16, 2, small_solid),
    "test_rgb_gradient.png": (64, 64, 2, rgb_gradient),
    "test_rgba_alpha.png": (100, 100, 6, rgba_alpha),
    "test_large.png": (2000, 1200, 2, large_gradient),
}


def generate(outdir: str) -> dict:
    """Writes every fixture into outdir, returns {name: path}."""
    os.makedirs(outdir, exist_ok=True)
    paths = {}
    for name, (w, h, ct, fn) in FIXTURES.items():
        path = os.path.join(outdir, name)
        write_png(path, w, h, ct, fn)
        paths[name] = path
    return paths


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    paths = generate(outdir)
    for name, path in paths.items():
        print(f"wrote {path} ({os.path.getsize(path)} bytes)")
