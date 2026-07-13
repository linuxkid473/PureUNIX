/* imgview -- native PNG image viewer for PureUNIX (docs/imgview.md).
 *
 * Decodes a real PNG file with real upstream libpng (which does its own
 * inflate via real upstream zlib -- see third_party/zlib, third_party/libpng)
 * and paints it on the framebuffer through the same pu_fb_*()/pu_input_poll()
 * syscall surface third_party/SDL2's platform backend uses
 * (user/pureunix_gfx.h, docs/sdl-port.md) -- no SDL dependency, and no
 * hand-rolled PNG decoding of any kind.
 *
 * The whole file is decoded into an ordinary RGBA8 heap buffer *before*
 * graphics mode is ever entered, so every "can't do this" case (missing
 * file, bad signature, unsupported/corrupt data, allocation failure) prints
 * a plain error to the still-text-mode console and exits -- imgview never
 * has to recover a half-drawn graphics screen on an error path. Once
 * graphics mode *is* entered, kernel/signal.c's force_graphics_mode_off_if_
 * owned() guarantees the console is restored even if this process is killed
 * outright, and PU_KEY_CTRL_S is the operator's own belt-and-suspenders
 * override for a hung app (kernel/vt.c) -- so the only cleanup imgview
 * itself is responsible for is the ordinary, voluntary exit path below.
 */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <png.h>

#include "pureunix_gfx.h"

struct image {
    unsigned char *pixels; /* width*height*4 bytes, row-major RGBA8 */
    unsigned int width;
    unsigned int height;
};

static void image_free(struct image *img)
{
    free(img->pixels);
    img->pixels = NULL;
}

static void pu_png_error(png_structp png_ptr, png_const_charp msg)
{
    fprintf(stderr, "imgview: PNG decode error: %s\n", msg);
    longjmp(png_jmpbuf(png_ptr), 1);
}

static void pu_png_warning(png_structp png_ptr, png_const_charp msg)
{
    (void)png_ptr;
    fprintf(stderr, "imgview: PNG warning: %s\n", msg);
}

/* Decodes `path` into *img as 8-bit-per-channel RGBA, regardless of the
 * PNG's own color type/bit depth/interlacing -- the standard libpng
 * "normalize everything to 8-bit RGBA" transform recipe from its own
 * documentation (libpng-manual.txt's "Reading PNG files incrementally"
 * example), not a bespoke conversion. Returns 0 on success, -1 with a
 * message already printed to stderr on any failure. */
static int load_png(const char *path, struct image *img)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "imgview: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    unsigned char sig[8];
    if (fread(sig, 1, sizeof(sig), fp) != sizeof(sig) || png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
        fprintf(stderr, "imgview: '%s' is not a valid PNG file\n", path);
        fclose(fp);
        return -1;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                                  pu_png_error, pu_png_warning);
    if (!png_ptr) {
        fprintf(stderr, "imgview: out of memory (png_create_read_struct)\n");
        fclose(fp);
        return -1;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "imgview: out of memory (png_create_info_struct)\n");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return -1;
    }

    png_bytep *row_pointers = NULL;
    memset(img, 0, sizeof(*img));

    if (setjmp(png_jmpbuf(png_ptr))) {
        /* Reached via longjmp from pu_png_error() (a real decode error, an
         * unsupported feature libpng itself rejects, or a libpng-internal
         * allocation failure) -- the message was already printed there. */
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(row_pointers);
        image_free(img);
        fclose(fp);
        return -1;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sizeof(sig));
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
                 &interlace_type, NULL, NULL);

    if (width == 0 || height == 0) {
        fprintf(stderr, "imgview: '%s' has zero width or height\n", path);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return -1;
    }

    /* Normalize every supported PNG color type/bit depth/transparency form
     * down to plain 8-bit RGBA -- RGB, RGBA, grayscale, grayscale+alpha,
     * palette (with or without tRNS), and 1/2/4/16-bit depths all end up in
     * exactly the same buffer shape below. */
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    /* Adam7 de-interlacing, if any -- png_read_image() below performs all
     * of the resulting passes itself; nothing else has to loop over them. */
    png_set_interlace_handling(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    if (rowbytes != (png_size_t)width * 4) {
        fprintf(stderr, "imgview: '%s' uses an unsupported PNG pixel format\n", path);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return -1;
    }

    unsigned char *pixels = malloc((size_t)rowbytes * height);
    if (!pixels) {
        fprintf(stderr, "imgview: out of memory allocating %ux%u image\n", width, height);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return -1;
    }

    row_pointers = malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        fprintf(stderr, "imgview: out of memory allocating row pointer table\n");
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return -1;
    }
    for (png_uint_32 y = 0; y < height; y++)
        row_pointers[y] = pixels + (size_t)y * rowbytes;

    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, NULL);

    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    img->pixels = pixels;
    img->width = width;
    img->height = height;
    return 0;
}

/* Packs one 8-bit-per-channel RGB triple into the framebuffer's native
 * pixel layout (fb->{red,green,blue}_{pos,size} -- see pu_fb_getinfo()'s
 * doc comment), the same bit-layout-driven approach SDL_puvideo.c uses to
 * pick a matching SDL_PixelFormatEnum rather than assuming any one hardware
 * layout. Channels narrower than 8 bits (e.g. 16bpp 5/6/5) are truncated by
 * a simple right-shift, which is exactly what real conversion from 8-bit
 * source data to a lower-precision framebuffer format has to do. */
static inline uint32_t pack_pixel(const pu_fb_info_t *fb, unsigned r, unsigned g, unsigned b)
{
    uint32_t px = 0;
    px |= (r >> (8 - fb->red_size)) << fb->red_pos;
    px |= (g >> (8 - fb->green_size)) << fb->green_pos;
    px |= (b >> (8 - fb->blue_size)) << fb->blue_pos;
    return px;
}

static void write_native_pixel(unsigned char *dst, unsigned int bypp, uint32_t px)
{
    /* x86 is little-endian; a tightly-packed low-to-high byte store here
     * matches every bypp pu_fb_getinfo() can report (SDL_pufb.c makes the
     * same assumption for this same syscall surface). */
    for (unsigned int i = 0; i < bypp; i++) {
        dst[i] = (unsigned char)(px >> (8 * i));
    }
}

/* Nearest-neighbour scale-to-fit (only ever shrinks, never enlarges -- a
 * smaller-than-screen image is centered at its native size instead) plus
 * centering, composited over a black backdrop with a plain alpha blend
 * (blending against black reduces to a straight multiply, so RGBA and
 * opaque RGB images share the same code path here). */
static void render_image(const struct image *img, const pu_fb_info_t *fb, unsigned char *fbbuf)
{
    unsigned int dst_w = img->width;
    unsigned int dst_h = img->height;
    if (img->width > fb->width || img->height > fb->height) {
        double sx = (double)fb->width / (double)img->width;
        double sy = (double)fb->height / (double)img->height;
        double scale = sx < sy ? sx : sy;
        dst_w = (unsigned int)(img->width * scale);
        dst_h = (unsigned int)(img->height * scale);
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
    }

    unsigned int off_x = (fb->width - dst_w) / 2;
    unsigned int off_y = (fb->height - dst_h) / 2;

    memset(fbbuf, 0, (size_t)fb->width * fb->height * fb->bypp);

    for (unsigned int dy = 0; dy < dst_h; dy++) {
        unsigned int sy = (unsigned int)((uint64_t)dy * img->height / dst_h);
        const unsigned char *srow = img->pixels + (size_t)sy * img->width * 4;
        unsigned char *drow = fbbuf + (size_t)(off_y + dy) * fb->width * fb->bypp +
                               (size_t)off_x * fb->bypp;
        for (unsigned int dx = 0; dx < dst_w; dx++) {
            unsigned int sx = (unsigned int)((uint64_t)dx * img->width / dst_w);
            const unsigned char *sp = srow + (size_t)sx * 4;
            unsigned int a = sp[3];
            unsigned int r = (sp[0] * a) / 255;
            unsigned int g = (sp[1] * a) / 255;
            unsigned int b = (sp[2] * a) / 255;
            uint32_t px = pack_pixel(fb, r, g, b);
            write_native_pixel(drow + (size_t)dx * fb->bypp, fb->bypp, px);
        }
    }
}

static int is_exit_key(const pu_input_event_t *ev)
{
    if (ev->type != PU_INPUT_KEY || ev->value == 0)
        return 0; /* only key-down events */
    return ev->code == PU_KEY_ESCAPE || ev->code == PU_KEY_CTRL_C ||
           ev->code == 'q' || ev->code == 'Q';
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: imgview <file.png>\n");
        return 1;
    }

    struct image img;
    if (load_png(argv[1], &img) != 0)
        return 1;

    pu_fb_info_t fb;
    if (pu_fb_getinfo(&fb) != 0) {
        fprintf(stderr, "imgview: no framebuffer available on this console\n");
        image_free(&img);
        return 1;
    }

    void *fbbuf = pu_fb_mmap();
    if (!fbbuf) {
        fprintf(stderr, "imgview: failed to map the framebuffer\n");
        image_free(&img);
        return 1;
    }

    printf("imgview: %s: %ux%u -> displaying on %ux%u framebuffer (any key / q / Ctrl+C to exit)\n",
           argv[1], img.width, img.height, (unsigned int)fb.width, (unsigned int)fb.height);

    pu_set_graphics_mode(1);
    render_image(&img, &fb, fbbuf);
    pu_fb_blit(fbbuf, fb.width * fb.height * fb.bypp);

    int quit = 0;
    while (!quit) {
        pu_input_event_t ev;
        int r;
        while ((r = pu_input_poll(&ev)) == 1) {
            if (is_exit_key(&ev)) {
                quit = 1;
                break;
            }
        }
        if (r < 0)
            break; /* input path gone away; don't spin forever */
        usleep(20000);
    }

    pu_set_graphics_mode(0);
    image_free(&img);
    return 0;
}
