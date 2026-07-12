#ifndef PUREUNIX_FRAMEBUFFER_H
#define PUREUNIX_FRAMEBUFFER_H

#include <pureunix/types.h>

typedef struct {
    bool present;
    uint32_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t red_pos, red_size;
    uint8_t green_pos, green_size;
    uint8_t blue_pos, blue_size;
} fb_info_t;

/* Userspace-visible ABI mirror of fb_info_t (SDL2's pureunix video backend,
 * docs/sdl-port.md) -- deliberately not fb_info_t itself: that struct's
 * bool/uint8_t mix has no guaranteed layout across a syscall boundary, the
 * same reason include/pureunix/stat.h's struct pureunix_stat exists
 * alongside the kernel's own inode-facing struct. Layout must match
 * user/pureunix_gfx.h's pu_fb_info_t.
 *
 * Deliberately exposes the *native* red/green/blue field layout rather
 * than normalizing to one fixed format: SYS_FB_BLIT expects the caller's
 * buffer already packed exactly this way (bypp bytes/pixel, these bit
 * positions/sizes), so it can copy it to VRAM with no per-pixel repacking
 * -- the same approach a real Linux fbdev-backed SDL port takes (query the
 * hardware's actual bit layout once via an ioctl, then build an
 * SDL_PixelFormat to match, rather than converting every blit). */
struct pureunix_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t bypp; /* bytes per pixel (fb.bpp/8): 3 or 4 */
    uint32_t red_pos, red_size;
    uint32_t green_pos, green_size;
    uint32_t blue_pos, blue_size;
};

/* Copies a tightly-packed (bypp bytes/pixel, no padding, row-major, no
 * hardware pitch) userspace pixel buffer into the real framebuffer,
 * honoring the hardware's own pitch internally -- SYS_FB_BLIT's
 * (arch/i386/syscall.c) implementation. buf must already be packed exactly
 * per struct pureunix_fb_info's field layout above (SYS_FB_GETINFO) and be
 * exactly fb.width * fb.height * bypp bytes (len is checked against that).
 * A plain per-row memcpy, not fb_put_pixel()'s per-pixel pack -- there is
 * nothing left to convert once the caller already matches the hardware's
 * own layout. Returns 0, -ENODEV (no framebuffer), or -EINVAL (len
 * mismatch). */
int fb_blit_buffer(const uint8_t *buf, size_t len);

/* Parses the multiboot2 framebuffer tag (if any). Safe to call before
 * paging is set up: it only reads mbi fields, never touches fb memory. */
bool fb_probe(uint32_t magic, uint32_t mbi_addr);

const fb_info_t *fb_get_info(void);

/* The following require the framebuffer's physical range to already be
 * mapped (see vmm_init) and fb_probe() to have found a usable mode. */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);

/* Starts mirroring the framebuffer in a kmalloc'd RAM buffer so fb_scroll_up()
 * below can stop reading VRAM on every scroll -- see the comment on the
 * `shadow` variable in drivers/framebuffer.c for why that read (not the
 * write) is what makes scrolling slow on real hardware. Requires kmalloc(),
 * so must be called after heap_init() (see kernel/main.c); no-op if called
 * again or if there's no framebuffer. Safe to skip: every function above
 * just keeps operating directly on VRAM, as they did before this existed. */
void fb_enable_shadow(void);

/* Shifts the sub-rectangle [x, x+w) x [y, y+h) up by pixel_rows, filling the
 * exposed bottom strip with bg_rgb. Bootloaders may grant a framebuffer
 * larger than requested, so callers scroll only their own region. */
void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel_rows, uint32_t bg_rgb);

#endif
