#include <pureunix/errno.h>
#include <pureunix/framebuffer.h>
#include <pureunix/memory.h>
#include <pureunix/multiboot.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

static fb_info_t fb;

/* RAM mirror of the framebuffer's bytes, same pitch/layout as the real VRAM
 * (see fb_enable_shadow()) so every offset formula below is identical for
 * both -- NULL until fb_enable_shadow() succeeds, in which case every
 * function here silently falls back to operating on VRAM directly, exactly
 * as before this shadow existed.
 *
 * Why this exists: on real hardware the linear framebuffer's PCI BAR is
 * mapped Write-Combining (see enable_pat_write_combining() in kernel/vmm.c)
 * so that a stream of small *writes* gets buffered into full-cacheline PCI
 * bursts instead of draining one at a time. WC does nothing for *reads*,
 * though -- a read from WC-mapped MMIO is still a real, uncached, blocking
 * round trip over the bus, same as UC. fb_scroll_up() used to implement
 * "shift the console up one line" as a memmove() directly over VRAM, which
 * reads roughly as many bytes as it writes.
 *
 * drivers/vga.c's PERF serial log (see perf_report() there) shows this cost
 * as an essentially constant ~8.5-9M cycles per scroll regardless of how
 * much text actually changed, which is consistent with "dominated by
 * moving a fixed ~4 MiB region" rather than by glyph count -- but that log
 * was captured under QEMU, where the virtual display is backed by ordinary
 * host RAM and a VRAM read is therefore just as cheap as a write. Real
 * silicon has no such shortcut: every one of those reads is a genuine
 * uncached PCI/PCIe round trip, which is what turns this same memmove into
 * the visibly-collapsing scroll performance reported on the HP Pavilion
 * (and why QEMU could never reproduce it). With this shadow, fb_scroll_up()
 * instead memmove()s within ordinary cached RAM (fast) and only ever
 * *writes* the result back to VRAM (fast under WC, same as every other
 * draw path already relies on) -- it never reads VRAM again after the
 * one-time copy in fb_enable_shadow(). */
static uint8_t *shadow;

/* Upper bound on how many physical frames fb_enable_shadow() will ever claim
 * -- 16 MiB, comfortably covering any realistic laptop panel (e.g. a real
 * 1920x1080x32bpp framebuffer needs ~8 MiB) while still leaving the great
 * majority of the low-128MiB PMM pool (see MAX_MEMORY_BYTES in
 * kernel/pmm.c) for everything that allocates from it *after* this runs:
 * xHCI's DCBAA/scratchpad/rings, e1000's descriptor rings, and the
 * fat.img/root.img GRUB modules already reserved before pmm_alloc_frame()
 * is ever called. Exceeding this cap just skips the shadow (see the
 * fallback path in fb_scroll_up()) rather than risk starving those. */
#define FB_SHADOW_MAX_FRAMES ((16U * 1024U * 1024U) / PUREUNIX_PAGE_SIZE)

/* Called once, after heap_init() (see kernel/main.c's boot order comment
 * above vga_init()/heap_init()), to start mirroring the framebuffer in RAM.
 *
 * Deliberately NOT kmalloc()'d: the kernel heap is a fixed 8 MiB arena (see
 * HEAP_SIZE in kernel/heap.c) shared with every other kernel allocation for
 * the rest of this boot and every login session after it. A framebuffer
 * mirror sized to the real, on-device panel resolution can be several MiB
 * on real hardware (QEMU's small virtual display made this look harmless in
 * testing) -- taking that out of the shared 8 MiB heap as the very first
 * post-heap_init() allocation starved later, unrelated allocations of heap
 * room they needed (e.g. ext2's read_file() kcalloc()'ing a full file into
 * memory to exec it -- see fs/ext2/file.c's "kcalloc failed" report). This
 * instead claims its own dedicated physical frames straight from the PMM,
 * the same way xHCI's DMA buffers do (see alloc_dma_page() in
 * drivers/xhci.c) specifically so it can never again compete with the
 * general-purpose heap for space. Every frame in the low 128MiB is already
 * identity-mapped 1:1 by vmm_init(), so a physical frame address doubles as
 * a directly usable kernel pointer with no extra vmm_map_page() call.
 *
 * Uses pmm_alloc_contiguous() (see kernel/pmm.c), not pmm_alloc_frame() in a
 * loop: this driver's needed run is typically past a large already-reserved
 * stretch (the kernel image, GRUB's fat.img/root.img modules, and the
 * kernel heap all sit back-to-back near the bottom of the low 128MiB —
 * see pmm_init() in kernel/pmm.c), and pmm_alloc_frame() itself is an
 * O(total_frames) scan from frame 0 on every call, so allocating N frames
 * one at a time to reach that run costs O(N * total_frames). That was
 * measured as a multi-second stall building this shadow's ~1000-frame
 * request under QEMU's software CPU emulation before being replaced with
 * the single-scan version.
 *
 * The one-time bulk read of real VRAM here, to prime the shadow with
 * whatever's already on screen (the boot banner, bootsplash remnants, ...),
 * pays exactly the same per-byte cost fb_scroll_up() used to pay on *every*
 * scroll -- but it happens exactly once, not once per line of boot/shell
 * output, so it's a non-issue.
 *
 * No-op (leaves the direct-VRAM fallback in fb_scroll_up() in place) if the
 * required size exceeds FB_SHADOW_MAX_FRAMES, the PMM can't supply a run
 * that long, or there's no framebuffer at all. */
void fb_enable_shadow(void)
{
    if (!fb.present || shadow) {
        return;
    }
    size_t needed = (size_t)fb.pitch * fb.height;
    uint32_t frames_needed = (uint32_t)((needed + PUREUNIX_PAGE_SIZE - 1) / PUREUNIX_PAGE_SIZE);
    if (frames_needed == 0 || frames_needed > FB_SHADOW_MAX_FRAMES) {
        printf("fb: shadow disabled: %u bytes (%u frames) exceeds the %u-frame cap -- "
               "falling back to direct-VRAM scrolling\n",
               (unsigned)needed, frames_needed, FB_SHADOW_MAX_FRAMES);
        return;
    }

    phys_addr_t base = pmm_alloc_contiguous(frames_needed);
    if (!base) {
        printf("fb: shadow disabled: no %u-frame contiguous run available -- "
               "falling back to direct-VRAM scrolling\n",
               frames_needed);
        return;
    }
    printf("fb: shadow enabled: %u bytes (%u frames) at phys=%p\n", (unsigned)needed,
           frames_needed, (void *)(uintptr_t)base);

    uint8_t *buf = (uint8_t *)(uintptr_t)base;
    memcpy(buf, (const void *)(uintptr_t)fb.addr, needed);
    shadow = buf;
}

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

/* Plain (non-volatile) store into the RAM shadow -- no PCI transaction to
 * economize on, so this is just "write the bytes"; kept as its own helper
 * so fb_put_pixel()/fb_fill_rect() don't have to duplicate the bpp==32
 * vs. bpp==24 branch for every VRAM write they already do. */
static inline void shadow_store_pixel(uint8_t *p, uint32_t value, uint32_t bypp)
{
    if (bypp == 4) {
        *(uint32_t *)p = value;
    } else {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16);
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!fb.present || x >= fb.width || y >= fb.height) {
        return;
    }
    uint32_t value = pack_pixel(rgb);
    uint32_t bypp = fb.bpp / 8;
    uint32_t off = y * fb.pitch + x * bypp;
    uint8_t *p = (uint8_t *)(uintptr_t)(fb.addr + off);
    if (fb.bpp == 32) {
        /* One 32-bit store instead of four separate byte stores: over the
         * PCI bus every store is its own transaction (see
         * enable_pat_write_combining() in kernel/vmm.c for why write-combine
         * mapping alone isn't the whole story) — 4x fewer transactions per
         * pixel matters even with WC enabled. */
        *(volatile uint32_t *)p = value;
    } else {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16);
    }
    if (shadow) {
        shadow_store_pixel(shadow + off, value, bypp);
    }
}

/* Bulk-fills [x, x+w) x [y, y+h) with a single packed color. Used for whole-
 * screen/whole-console clears and for the row exposed by fb_scroll_up() —
 * both want "set this many bytes of VRAM to a constant pattern", not
 * per-pixel blending, so this bypasses fb_put_pixel()'s per-call bounds
 * check and address recomputation and instead paints each row through a
 * plain pointer, one store per pixel (one store per 4 bytes at 32bpp) with
 * the row base computed once. That's the difference between "render N
 * glyphs to blank them" (what vga_clear() used to do) and "write N*bypp
 * bytes" — see vga_clear() in drivers/vga.c. */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!fb.present) {
        return;
    }
    if (x >= fb.width || y >= fb.height) {
        return;
    }
    if (x + w > fb.width) {
        w = fb.width - x;
    }
    if (y + h > fb.height) {
        h = fb.height - y;
    }
    uint32_t value = pack_pixel(rgb);
    uint32_t bypp = fb.bpp / 8;
    for (uint32_t row = 0; row < h; ++row) {
        uint32_t off = (y + row) * fb.pitch + x * bypp;
        uint8_t *p = (uint8_t *)(uintptr_t)(fb.addr + off);
        if (fb.bpp == 32) {
            volatile uint32_t *dst = (volatile uint32_t *)p;
            for (uint32_t col = 0; col < w; ++col) {
                dst[col] = value;
            }
        } else {
            for (uint32_t col = 0; col < w; ++col) {
                p[0] = (uint8_t)value;
                p[1] = (uint8_t)(value >> 8);
                p[2] = (uint8_t)(value >> 16);
                p += bypp;
            }
        }
        if (shadow) {
            uint8_t *sp = shadow + off;
            for (uint32_t col = 0; col < w; ++col) {
                shadow_store_pixel(sp, value, bypp);
                sp += bypp;
            }
        }
    }
}

void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel_rows, uint32_t bg_rgb)
{
    if (!fb.present || pixel_rows >= h) {
        return;
    }
    uint32_t bypp = fb.bpp / 8;

    if (!shadow) {
        /* Fallback for the narrow early-boot window before fb_enable_shadow()
         * has run (see its comment) -- reads VRAM directly, exactly the old
         * behavior. Never hit once the console can actually fill a screen
         * and need to scroll. */
        for (uint32_t ry = 0; ry < h - pixel_rows; ++ry) {
            uint8_t *dst = (uint8_t *)(uintptr_t)(fb.addr + (y + ry) * fb.pitch + x * bypp);
            uint8_t *src = (uint8_t *)(uintptr_t)(fb.addr + (y + ry + pixel_rows) * fb.pitch + x * bypp);
            memmove(dst, src, w * bypp);
        }
        fb_fill_rect(x, y + h - pixel_rows, w, pixel_rows, bg_rgb);
        return;
    }

    /* Shift and blank entirely within the RAM shadow first -- ordinary
     * cached reads and writes, not VRAM round trips -- then push the result
     * to VRAM as a single write-only pass per row. This is the same net
     * effect as the old direct-VRAM version (same bytes end up moved and
     * cleared in the real framebuffer) but it never *reads* VRAM, which is
     * the half of that operation real hardware can't do quickly (see the
     * comment above the `shadow` declaration). */
    uint32_t value = pack_pixel(bg_rgb);
    for (uint32_t ry = 0; ry < h - pixel_rows; ++ry) {
        uint8_t *dst = shadow + (y + ry) * fb.pitch + x * bypp;
        uint8_t *src = shadow + (y + ry + pixel_rows) * fb.pitch + x * bypp;
        memmove(dst, src, w * bypp);
    }
    for (uint32_t ry = h - pixel_rows; ry < h; ++ry) {
        uint8_t *sp = shadow + (y + ry) * fb.pitch + x * bypp;
        for (uint32_t col = 0; col < w; ++col) {
            shadow_store_pixel(sp, value, bypp);
            sp += bypp;
        }
    }
    for (uint32_t ry = 0; ry < h; ++ry) {
        uint32_t off = (y + ry) * fb.pitch + x * bypp;
        memcpy((void *)(uintptr_t)(fb.addr + off), shadow + off, w * bypp);
    }
}

int fb_blit_buffer(const uint8_t *buf, size_t len)
{
    if (!fb.present) {
        return -ENODEV;
    }
    uint32_t bypp = fb.bpp / 8;
    size_t expected = (size_t)fb.width * fb.height * bypp;
    if (!buf || len != expected) {
        return -EINVAL;
    }

    size_t row_bytes = (size_t)fb.width * bypp;
    for (uint32_t row = 0; row < fb.height; ++row) {
        uint32_t off = row * fb.pitch;
        memcpy((void *)(uintptr_t)(fb.addr + off), buf + (size_t)row * row_bytes, row_bytes);
        if (shadow) {
            memcpy(shadow + off, buf + (size_t)row * row_bytes, row_bytes);
        }
    }
    return 0;
}
