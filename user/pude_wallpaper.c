/* user/pude_wallpaper.c -- see pude_wallpaper.h.
 *
 * PNG decode is the exact same libpng recipe user/imgview.c already uses
 * (the standard "normalize everything to 8-bit RGBA" transform chain from
 * libpng's own manual) -- duplicated rather than shared because imgview is
 * a separate freestanding ELF built without SDL at all, while this file is
 * one more translation unit linked into the SDL-based `pude` binary.
 */
#include "pude_wallpaper.h"

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#define PUDE_CONF_PATH "/etc/pude.conf"

/* ---- decoded-PNG scratch buffer (freed again as soon as it's scaled into
 * the cache below) ---- */
typedef struct {
    unsigned char *pixels; /* width*height*4 bytes, row-major RGBA8 */
    unsigned int width;
    unsigned int height;
} wp_image_t;

/* ---- module state -- real shared globals, same convention as g_request
 * in user/pude_spawn.c ---- */
static struct {
    Uint32 *pixels; /* screen_w*height native-format pixels, or NULL */
    int w, h;
} g_cache;

static const SDL_PixelFormat *g_screen_fmt;
static int g_screen_w, g_screen_h;

static char g_current_path[PUDE_WALLPAPER_PATH_MAX];
static bool g_has_path;

/* ---- PNG decode (see user/imgview.c's load_png() for the annotated
 * version of this same recipe) ---- */

static void wp_png_error(png_structp png_ptr, png_const_charp msg)
{
    fprintf(stderr, "pude: wallpaper PNG decode error: %s\n", msg);
    longjmp(png_jmpbuf(png_ptr), 1);
}

static void wp_png_warning(png_structp png_ptr, png_const_charp msg)
{
    (void)png_ptr;
    fprintf(stderr, "pude: wallpaper PNG warning: %s\n", msg);
}

static bool wp_load_png(const char *path, wp_image_t *img)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "pude: cannot open wallpaper '%s': %s\n", path, strerror(errno));
        return false;
    }

    unsigned char sig[8];
    if (fread(sig, 1, sizeof(sig), fp) != sizeof(sig) || png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
        fprintf(stderr, "pude: '%s' is not a valid PNG file\n", path);
        fclose(fp);
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                                  wp_png_error, wp_png_warning);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return false;
    }

    png_bytep *row_pointers = NULL;
    memset(img, 0, sizeof(*img));

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(row_pointers);
        free(img->pixels);
        img->pixels = NULL;
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sizeof(sig));
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
                 &interlace_type, NULL, NULL);

    if (width == 0 || height == 0) {
        fprintf(stderr, "pude: wallpaper '%s' has zero width or height\n", path);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

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
    png_set_interlace_handling(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    if (rowbytes != (png_size_t)width * 4) {
        fprintf(stderr, "pude: wallpaper '%s' uses an unsupported PNG pixel format\n", path);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    unsigned char *pixels = malloc((size_t)rowbytes * height);
    if (!pixels) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }
    row_pointers = malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
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
    return true;
}

/* ---- aspect-fill scale into a cache buffer sized (sw x sh), one-time
 * nearest-neighbour resample (imgview.c's own render_image() scales
 * aspect-*fit*, letterboxing instead of filling -- a desktop wallpaper
 * wants the opposite: cover the whole screen, cropping whichever axis
 * overflows). Any transparency in the source PNG is blended against the
 * same solid color user/pude.c's render_frame() falls back to, so a
 * wallpaper with genuine alpha never shows through to raw uninitialized
 * pixels. ---- */
static void wp_scale_fill(const wp_image_t *img, Uint32 *out, int sw, int sh,
                           const SDL_PixelFormat *fmt)
{
    const unsigned int bg_r = 20, bg_g = 24, bg_b = 34;

    double sx = (double)sw / (double)img->width;
    double sy = (double)sh / (double)img->height;
    double scale = sx > sy ? sx : sy; /* cover, not fit */

    int scaled_w = (int)(img->width * scale + 0.5);
    int scaled_h = (int)(img->height * scale + 0.5);
    if (scaled_w < 1) scaled_w = 1;
    if (scaled_h < 1) scaled_h = 1;
    int off_x = (scaled_w - sw) / 2;
    int off_y = (scaled_h - sh) / 2;

    for (int dy = 0; dy < sh; dy++) {
        int sy_i = (int)(((int64_t)(dy + off_y) * img->height) / scaled_h);
        if (sy_i < 0) sy_i = 0;
        if (sy_i >= (int)img->height) sy_i = (int)img->height - 1;
        const unsigned char *srow = img->pixels + (size_t)sy_i * img->width * 4;
        Uint32 *drow = out + (size_t)dy * sw;

        for (int dx = 0; dx < sw; dx++) {
            int sx_i = (int)(((int64_t)(dx + off_x) * img->width) / scaled_w);
            if (sx_i < 0) sx_i = 0;
            if (sx_i >= (int)img->width) sx_i = (int)img->width - 1;
            const unsigned char *sp = srow + (size_t)sx_i * 4;

            unsigned int a = sp[3];
            unsigned int r = (sp[0] * a + bg_r * (255 - a)) / 255;
            unsigned int g = (sp[1] * a + bg_g * (255 - a)) / 255;
            unsigned int b = (sp[2] * a + bg_b * (255 - a)) / 255;
            drow[dx] = SDL_MapRGB(fmt, (Uint8)r, (Uint8)g, (Uint8)b);
        }
    }
}

/* ---- config file (docs/pude.md's "Configuration file" section) -- a
 * single `key=value` line, the smallest format that can express one
 * setting; extending it later (a second setting) only ever needs a second
 * `if (strncmp(...))` in wp_read_config_path(), not a format change. ---- */

static void wp_read_config_path(char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen(PUDE_CONF_PATH, "r");
    if (!f) {
        return;
    }
    char line[PUDE_WALLPAPER_PATH_MAX + 16];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "wallpaper=", 10) == 0) {
            strncpy(out, line + 10, cap - 1);
            out[cap - 1] = '\0';
            break;
        }
    }
    fclose(f);
}

static bool wp_write_config_path(const char *path)
{
    FILE *f = fopen(PUDE_CONF_PATH, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "wallpaper=%s\n", path);
    fclose(f);
    return true;
}

/* ---- shared apply path for both pude_wallpaper_init() (persist=false --
 * the path already came from the config file) and pude_wallpaper_set()
 * (persist=true) ---- */
static bool wp_apply(const char *path, bool persist)
{
    wp_image_t img;
    if (!wp_load_png(path, &img)) {
        return false;
    }

    Uint32 *newbuf = malloc((size_t)g_screen_w * (size_t)g_screen_h * sizeof(Uint32));
    if (!newbuf) {
        free(img.pixels);
        return false;
    }
    wp_scale_fill(&img, newbuf, g_screen_w, g_screen_h, g_screen_fmt);
    free(img.pixels);

    free(g_cache.pixels);
    g_cache.pixels = newbuf;
    g_cache.w = g_screen_w;
    g_cache.h = g_screen_h;

    strncpy(g_current_path, path, sizeof(g_current_path) - 1);
    g_current_path[sizeof(g_current_path) - 1] = '\0';
    g_has_path = true;

    if (persist) {
        wp_write_config_path(path);
    }
    return true;
}

void pude_wallpaper_init(SDL_Surface *screen)
{
    g_screen_fmt = screen->format;
    g_screen_w = screen->w;
    g_screen_h = screen->h;

    char path[PUDE_WALLPAPER_PATH_MAX];
    wp_read_config_path(path, sizeof(path));
    if (path[0]) {
        wp_apply(path, false);
    }
}

bool pude_wallpaper_set(const char *path)
{
    return wp_apply(path, true);
}

const char *pude_wallpaper_current_path(void)
{
    return g_has_path ? g_current_path : NULL;
}

void pude_wallpaper_render(SDL_Surface *s)
{
    if (!g_cache.pixels || g_cache.w != s->w || g_cache.h != s->h) {
        SDL_Rect full = { 0, 0, s->w, s->h };
        SDL_FillRect(s, &full, SDL_MapRGB(s->format, 20, 24, 34));
        return;
    }
    if (s->pitch == s->w * 4) {
        memcpy(s->pixels, g_cache.pixels, (size_t)s->w * (size_t)s->h * 4);
    } else {
        for (int y = 0; y < s->h; y++) {
            memcpy((Uint8 *)s->pixels + (size_t)y * s->pitch,
                   (const Uint8 *)g_cache.pixels + (size_t)y * s->w * 4, (size_t)s->w * 4);
        }
    }
}
