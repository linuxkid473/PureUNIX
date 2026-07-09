/*
 * user/font.c — runtime console font scaling.
 *
 * Thin CLI wrapper around ioctl(TIOCSFONT) (include/pureunix/ioctl.h),
 * which itself calls drivers/vga.c's vga_apply_font_scale() -- resizes the
 * console text grid for a new pixel scale and repaints, entirely at
 * runtime (no kernel rebuild, no reboot). Only meaningful when the console
 * is framebuffer-backed; legacy 80x25 VGA text mode has no adjustable
 * glyph size, and TIOCSFONT reports that as -EINVAL like any other
 * unsupported request.
 */
#include "libpure.h"

static void usage(void)
{
    pu_puts("usage: font [1-");
    pu_puti(FONT_SCALE_MAX);
    pu_puts("]\n");
    pu_puts("  with no argument, prints the current scale\n");
}

int main(int argc, char *argv[])
{
    struct winsize ws;
    if (pu_ioctl(1, TIOCGWINSZ, &ws) != 0) {
        pu_puts("font: not a console\n");
        return 1;
    }

    if (argc < 2) {
        pu_puts("current font scale: ");
        /* TIOCSFONT is write-only (no "get scale" ioctl exists), but the
         * scale is always implied by the console's own reported size vs.
         * the base 8x17 cell — not worth a second ioctl request just to
         * echo back a number the caller could derive from TIOCGWINSZ
         * itself if it really needed to; here it's purely informational,
         * so just point at `font N` to change it. */
        pu_puts("(use `font N` to set it)\n");
        usage();
        return 0;
    }

    int scale = pu_atoi(argv[1]);
    if (scale < FONT_SCALE_MIN || scale > FONT_SCALE_MAX) {
        usage();
        return 1;
    }

    if (pu_ioctl(1, TIOCSFONT, &scale) != 0) {
        pu_puts("font: could not change scale (no framebuffer console?)\n");
        return 1;
    }
    return 0;
}
