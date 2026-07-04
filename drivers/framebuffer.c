#include <pureunix/framebuffer.h>
#include <pureunix/multiboot.h>
#include <pureunix/string.h>

static fb_info_t fb;

static void parse_multiboot2(uint32_t mbi_addr)
{
    uint32_t total_size = *(uint32_t *)mbi_addr;
    uint8_t *cursor = (uint8_t *)(mbi_addr + 8);
    uint8_t *end = (uint8_t *)(mbi_addr + total_size);

    while (cursor < end) {
        multiboot2_tag_t *tag = (multiboot2_tag_t *)cursor;
        if (tag->type == 0) {
            break;
        }
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            multiboot2_framebuffer_tag_t *fbtag = (multiboot2_framebuffer_tag_t *)tag;
            if (fbtag->framebuffer_type == MULTIBOOT2_FRAMEBUFFER_TYPE_RGB &&
                (fbtag->framebuffer_bpp == 32 || fbtag->framebuffer_bpp == 24) &&
                fbtag->framebuffer_addr <= 0xFFFFFFFFULL) {
                multiboot2_framebuffer_rgb_info_t *rgb =
                    (multiboot2_framebuffer_rgb_info_t *)(fbtag + 1);
                fb.present = true;
                fb.addr = (uint32_t)fbtag->framebuffer_addr;
                fb.pitch = fbtag->framebuffer_pitch;
                fb.width = fbtag->framebuffer_width;
                fb.height = fbtag->framebuffer_height;
                fb.bpp = fbtag->framebuffer_bpp;
                fb.red_pos = rgb->red_field_position;
                fb.red_size = rgb->red_mask_size;
                fb.green_pos = rgb->green_field_position;
                fb.green_size = rgb->green_mask_size;
                fb.blue_pos = rgb->blue_field_position;
                fb.blue_size = rgb->blue_mask_size;
            }
        }
        cursor += ALIGN_UP(tag->size, 8);
    }
}

bool fb_probe(uint32_t magic, uint32_t mbi_addr)
{
    memset(&fb, 0, sizeof(fb));
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        parse_multiboot2(mbi_addr);
    }
    return fb.present;
}

const fb_info_t *fb_get_info(void)
{
    return &fb;
}

static inline uint32_t pack_pixel(uint32_t rgb)
{
    uint8_t r = (uint8_t)(rgb >> 16);
    uint8_t g = (uint8_t)(rgb >> 8);
    uint8_t b = (uint8_t)rgb;
    uint32_t rv = fb.red_size >= 8 ? r : (r >> (8 - fb.red_size));
    uint32_t gv = fb.green_size >= 8 ? g : (g >> (8 - fb.green_size));
    uint32_t bv = fb.blue_size >= 8 ? b : (b >> (8 - fb.blue_size));
    return (rv << fb.red_pos) | (gv << fb.green_pos) | (bv << fb.blue_pos);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!fb.present || x >= fb.width || y >= fb.height) {
        return;
    }
    uint32_t value = pack_pixel(rgb);
    uint8_t *p = (uint8_t *)(uintptr_t)(fb.addr + y * fb.pitch + x * (fb.bpp / 8));
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    if (fb.bpp == 32) {
        p[3] = (uint8_t)(value >> 24);
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!fb.present) {
        return;
    }
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            fb_put_pixel(x + col, y + row, rgb);
        }
    }
}

void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel_rows, uint32_t bg_rgb)
{
    if (!fb.present || pixel_rows >= h) {
        return;
    }
    uint32_t bypp = fb.bpp / 8;
    for (uint32_t ry = 0; ry < h - pixel_rows; ++ry) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(fb.addr + (y + ry) * fb.pitch + x * bypp);
        uint8_t *src = (uint8_t *)(uintptr_t)(fb.addr + (y + ry + pixel_rows) * fb.pitch + x * bypp);
        memmove(dst, src, w * bypp);
    }
    fb_fill_rect(x, y + h - pixel_rows, w, pixel_rows, bg_rgb);
}
