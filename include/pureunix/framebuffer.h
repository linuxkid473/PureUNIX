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

/* Parses the multiboot2 framebuffer tag (if any). Safe to call before
 * paging is set up: it only reads mbi fields, never touches fb memory. */
bool fb_probe(uint32_t magic, uint32_t mbi_addr);

const fb_info_t *fb_get_info(void);

/* The following require the framebuffer's physical range to already be
 * mapped (see vmm_init) and fb_probe() to have found a usable mode. */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);

/* Shifts the sub-rectangle [x, x+w) x [y, y+h) up by pixel_rows, filling the
 * exposed bottom strip with bg_rgb. Bootloaders may grant a framebuffer
 * larger than requested, so callers scroll only their own region. */
void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel_rows, uint32_t bg_rgb);

#endif
