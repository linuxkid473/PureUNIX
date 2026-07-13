# imgview

## Overview

`imgview` is a native PureUNIX PNG image viewer, `user/imgview.c` — decodes
a real PNG file with real, unmodified upstream libpng/zlib (`docs/libpng-
port.md`, `docs/zlib-port.md`) and paints it on the real framebuffer
through the same `pu_fb_*()`/`pu_input_poll()` syscall surface
(`user/pureunix_gfx.h`) SDL2's own platform backend uses
(`docs/sdl-port.md`) — no SDL dependency, and no hand-rolled PNG decoding
of any kind. Installed at `/bin/imgview.elf` with a plain `/bin/imgview`
symlink (`tools/mkext2.py`'s `add_bin()`, the same "hello -> hello.elf"
pattern every other PATH command here uses), so from a normal BusyBox ash
prompt:

```
# imgview /home/photo.png
imgview: /home/photo.png: 800x600 -> displaying on 1280x800 framebuffer (any key / q / Ctrl+C to exit)
(the real decoded image, scaled/centered as needed, on the real framebuffer)
# (any key, q, or Ctrl+C exits -- shell prompt reappears exactly as it was)
```

## Architecture

### Decode fully before ever touching graphics mode

`load_png()` decodes the entire file into an ordinary heap `RGBA8` buffer
*before* `pu_set_graphics_mode(1)` is ever called. Every "can't do this"
case — missing file, not-a-PNG, corrupt/unsupported data, an allocation
failure, a zero-width/height `IHDR` — prints a plain error to the
still-text-mode console and returns non-zero, so `imgview` never has to
recover a half-drawn graphics screen on an error path:

```
# imgview /home/missing.png
imgview: cannot open '/home/missing.png': No such file or directory
# imgview /home/not-a-png.txt
imgview: '/home/not-a-png.txt' is not a valid PNG file
# imgview
usage: imgview <file.png>
```

libpng's own error/warning callbacks (`pu_png_error`/`pu_png_warning`,
installed via `png_create_read_struct`) print libpng's real diagnostic
message and `longjmp` back to a `setjmp(png_jmpbuf(png_ptr))` in
`load_png()`, which cleans up and returns -1 — the standard libpng
error-handling pattern, not a bespoke one. This is also what catches
libpng-internal allocation failures (a real `malloc()` failure inside
libpng itself calls the same error callback).

### Normalizing every PNG to 8-bit RGBA

`load_png()` applies libpng's own documented "normalize any PNG to 8-bit
RGBA" transform recipe (`png_set_palette_to_rgb`,
`png_set_expand_gray_1_2_4_to_8`, `png_set_tRNS_to_alpha`,
`png_set_strip_16`, `png_set_gray_to_rgb`, `png_set_add_alpha`,
`png_set_interlace_handling`) so every supported PNG — palette, grayscale,
grayscale+alpha, RGB, RGBA; 1/2/4/8/16-bit depth; interlaced or not — ends
up in exactly the same `width*height*4`-byte buffer shape.
`png_read_image()` performs every Adam7 pass itself; nothing in `imgview`
has to loop over them.

### Scale-to-fit, center, alpha-blend

`render_image()` never *enlarges* a smaller-than-screen image — it centers
it at native size (`off_x`/`off_y` computed from `(fb->width - dst_w) / 2`
etc.). A larger-than-screen image is nearest-neighbor downscaled by the
smaller of the two axis ratios (`min(fb.width/img.width,
fb.height/img.height)`), preserving aspect ratio, then centered the same
way — the unused axis is real letterboxing (left as the cleared black
backdrop), not stretched. Every pixel is alpha-blended against that black
backdrop with a plain integer multiply (`out = src * alpha / 255` — blending
against black reduces exactly to that, so opaque RGB and translucent RGBA
share the same code path with `alpha` simply always `255` for the former).

### The framebuffer boundary: `pu_fb_*()`, not kernel internals

`imgview` never touches kernel internals directly — it uses exactly the
same narrow, documented syscall surface (`user/pureunix_gfx.h`) SDL2's
`src/video/pureunix/` backend uses:

- `pu_fb_getinfo()` — real hardware geometry, including the native red/
  green/blue bit layout `pack_pixel()` packs into (so `imgview` never
  assumes any one hardware pixel format, the same reasoning
  `PUREUNIX_VideoInit()` uses to pick a matching `SDL_PixelFormatEnum`).
- `pu_fb_mmap()` — a dedicated window-surface buffer, rendered into
  directly (no separate scratch buffer + copy).
- `pu_fb_blit(buf, len)` — one whole-frame blit to real VRAM after
  rendering.
- `pu_input_poll()` — a non-blocking event poll for the exit key (Escape,
  `q`/`Q`, or Ctrl+C — `is_exit_key()`), with a `usleep(20000)` between
  polls so the loop doesn't busy-spin the cooperative scheduler.
- `pu_set_graphics_mode(1)`/`(0)` — entered right before the first render,
  left right before the normal-exit `return`.

No new syscalls, no new kernel APIs — `imgview` is a second real consumer
of the exact surface the SDL2 port already established, proving that
surface is genuinely reusable by a non-SDL graphics program.

### Clean exit and Ctrl+C/kill safety

`imgview` itself only has to handle its own *voluntary* exit path
(`pu_set_graphics_mode(0)` before `return`). It does **not** need its own
`SIGINT`/`SIGTERM` handler for safety: while a VT is in graphics mode, a
keypress goes to the raw input queue (`pu_input_poll()`), not the signal
path, so Ctrl+C inside `imgview` is just another key `is_exit_key()`
recognizes — and for actual process termination from *outside* (a `kill`
from another VT, or the operator's own `PU_KEY_CTRL_S` graphics-mode
killswitch sending `SIGKILL`), `kernel/signal.c`'s
`force_graphics_mode_off_if_owned()` already forces graphics mode off and
repaints the console for *any* default-terminate signal, not just
`SIGKILL` — a real, general kernel guarantee this port relies on rather
than duplicates.

## Testing

`tools/test-imgview.py` (run via `python3 tools/test-imgview.py` after
`make iso`) is a real, end-to-end proof, not a smoke test:

1. **Fixtures**: `tools/gen-test-pngs.py` hand-builds four real PNG files
   with no PIL/ImageMagick dependency (only the stdlib `zlib` module) —
   `test_tiny.png` (16×16 RGB, smaller than any real screen),
   `test_rgb_gradient.png` (64×64 opaque RGB, a pure function of (x, y)),
   `test_rgba_alpha.png` (100×100 RGBA with a genuine alpha ramp over a
   checkerboard), `test_large.png` (2000×1200 RGB, larger than any real
   framebuffer on both axes). Every one was independently verified against
   macOS's own `sips`/ImageIO decoder (a completely different PNG
   implementation) for correct dimensions/alpha before ever reaching the
   target.
2. **Scratch disk**: builds a scratch copy of the exact persistent EXT2 +
   MBR/GRUB image `make iso` produces (the real `mkext2.py`/`mkdiskimg.py`
   invocations, extracted live via `make -n -W ... <target>` so this can't
   silently drift out of sync with the real build) with those fixtures
   added at `/home/*.png`.
3. **Real pixel verification**: boots that image exactly the way `make
   run` does (`-drive ...,format=raw -boot c`, real MBR+GRUB — not a
   ramdisk), types `imgview /home/X.png` at the ash prompt, and uses
   QEMU's own `screendump` HMP command to capture the *actual* framebuffer
   contents to a PPM file. `predict_pixel()` replicates `render_image()`'s
   scale/center/alpha-blend integer math in Python and computes the exact
   expected color at 7 sampled coordinates (all four corners, center, and
   two quarter-points) per image; every one of the 28 samples across all
   four fixtures matched, confirming RGB decode, RGBA/alpha-blend decode,
   aspect-ratio-preserving downscale, and centering all render correct
   real pixels — not just that the process exits 0.
4. **Error paths**: missing file, a corrupt/non-PNG file, and no arguments
   each produce the expected error message with the shell still
   responsive immediately after (`echo` round-trips right after each,
   proving no stuck graphics-mode/hung-process state).
5. **Keyboard exit**: after each successful render, a `q` keypress is sent
   and the ash prompt is confirmed to reappear — proving the console is
   genuinely restored, not just that the process exited.
6. **Persistence across reboot**: the same on-disk image is hard-killed
   (`SIGTERM`, no clean shutdown — the same "genuinely separate OS
   process, a false pass is essentially impossible" technique
   `tools/test-persistent-boot.py` uses) and booted again in a brand-new
   QEMU process; `test_tiny.png` still decodes and renders an identical,
   correct center pixel — proving the PNG file and `imgview` itself both
   survive a real reboot on the exact `make iso` artifact, not an
   ephemeral ramdisk.
7. **No regressions**: `make run-test` (`user/systest.c`'s regression
   suite) was re-run separately after this port and is still 343/345 — the
   same 2 pre-existing console-geometry failures as before, no new
   failures. (`test-imgview.py` deliberately does *not* also run
   `systest.elf` against its own scratch disk: that disk is a single-
   EXT2-partition MBR/GRUB image with no companion FAT16 volume, unlike
   `$(LIVE_ISO)`, so `systest.c`'s ~10 FAT16-specific checks would always
   fail there regardless of any change — a boot-mode mismatch, not a real
   regression signal.)

Screendumps from an actual run (100×100 RGBA with alpha ramp + checkerboard,
centered; 2000×1200 RGB, downscaled preserving aspect ratio and
letterboxed) were captured and visually match the fixtures' known content.

## Limitations

- Nearest-neighbor scaling only (no bilinear/Lanczos) — correct and
  simple, per the task's own "a simple nearest-neighbour scaler is
  acceptable initially if implemented correctly" allowance.
- No animated PNG (`acTL`/`fdAT`) support — libpng itself only ever decodes
  the default `IDAT` image, ignoring `acTL`/`fdAT` as unknown chunks
  (standard libpng behavior, not a gap introduced by this port).
- One image per invocation, no slideshow/next-image UI — matches the
  task's scope (`imgview FILE`, view, exit).
