/*
 * user/libexiftest.c — real on-target regression test for the vendored
 * libexif cross-build (third_party/libexif/i686-elf,
 * tools/build-libexif.sh). Third new dependency for the PCManFM-Qt port
 * (docs/pcmanfm-port.md phase 6) — libfm-qt uses it for JPEG EXIF
 * orientation when generating thumbnails.
 *
 * Not just "does it link" — genuinely parses a real JPEG file's EXIF
 * metadata through exif_data_new_from_file()/exif_content_get_entry()/
 * exif_entry_get_value() against a real fixture
 * (/pentax-exif-test.jpg, a real camera-shot JPEG from libexif's own
 * upstream test suite with a genuine Pentax MakerNote), the same class
 * of on-target check every other vendored dependency in this repo gets
 * (see user/libctest.c/user/libffitest.c/user/pcre2test_pu.c).
 *
 * Same harness convention as systest.c/libctest.c: every check is
 * independent and numbered, a failure never stops the run, a summary
 * prints at the end.
 */
#include <libexif/exif-data.h>
#include <stdio.h>
#include <string.h>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    printf("[%03d] %s\n", g_num, desc);
}

static void t_check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("      PASS: %s\n", what);
    } else {
        g_fail++;
        printf("      FAIL: %s\n", what);
    }
}

int main(void)
{
    ExifData *ed = NULL;

    t_begin("exif_data_new_from_file(): real JPEG + EXIF parse");
    {
        ed = exif_data_new_from_file("/pentax-exif-test.jpg");
        t_check(ed != NULL, "exif_data_new_from_file() succeeds on a real JPEG");
    }

    t_begin("exif_content_get_entry(EXIF_TAG_MAKE): real tag lookup");
    {
        char buf[128];
        ExifEntry *e = ed ? exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE) : NULL;
        t_check(e != NULL, "EXIF_TAG_MAKE entry found in IFD0");
        if (e) {
            exif_entry_get_value(e, buf, sizeof(buf));
            t_check(strstr(buf, "Asahi") != NULL || strstr(buf, "PENTAX") != NULL,
                    "Make tag value mentions Asahi/PENTAX");
        }
    }

    t_begin("exif_content_get_entry(EXIF_TAG_MODEL): real tag lookup");
    {
        char buf[128];
        ExifEntry *e = ed ? exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL) : NULL;
        t_check(e != NULL, "EXIF_TAG_MODEL entry found in IFD0");
        if (e) {
            exif_entry_get_value(e, buf, sizeof(buf));
            t_check(strstr(buf, "PENTAX") != NULL, "Model tag value mentions PENTAX");
        }
    }

    t_begin("exif_data_get_byte_order(): real byte-order detection");
    {
        if (ed) {
            ExifByteOrder bo = exif_data_get_byte_order(ed);
            t_check(bo == EXIF_BYTE_ORDER_INTEL || bo == EXIF_BYTE_ORDER_MOTOROLA,
                    "byte order is a real, valid value");
        } else {
            t_check(0, "byte order is a real, valid value");
        }
    }

    t_begin("exif_data_new_from_file(): honest failure on a non-JPEG path");
    {
        ExifData *bad = exif_data_new_from_file("/nonexistent-file-for-real.jpg");
        t_check(bad == NULL, "a missing file produces a real NULL, not a crash");
        if (bad) {
            exif_data_unref(bad);
        }
    }

    if (ed) {
        exif_data_unref(ed);
    }

    printf("\nlibexiftest: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
