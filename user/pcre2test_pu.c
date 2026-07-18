/*
 * user/pcre2test_pu.c — real on-target regression test for the vendored
 * PCRE2 cross-build (third_party/pcre2/i686-elf, tools/build-pcre2.sh).
 * Second new dependency for the PCManFM-Qt port (docs/pcmanfm-port.md
 * phase 5) — GLib's GRegex needs it.
 *
 * Named pcre2test_pu (not pcre2test) to avoid any confusion with
 * upstream PCRE2's own pcre2test command-line tool, which this is not
 * (that tool needs libreadline/libedit and isn't built at all here — see
 * tools/build-pcre2.sh's own comment).
 *
 * Not just "does it link" — genuinely compiles and matches real regular
 * expressions through pcre2_compile()/pcre2_match(), including capture
 * groups and a real "no match" case, the same class of on-target check
 * every other vendored dependency in this repo gets (see
 * user/libctest.c/user/libffitest.c).
 *
 * Same harness convention as systest.c/libctest.c: every check is
 * independent and numbered, a failure never stops the run, a summary
 * prints at the end.
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
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
    t_begin("pcre2_compile() + pcre2_match(): a simple literal match");
    {
        int errorcode;
        PCRE2_SIZE erroroffset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)"world", PCRE2_ZERO_TERMINATED, 0,
                                        &errorcode, &erroroffset, NULL);
        t_check(re != NULL, "pcre2_compile() succeeds for a valid pattern");

        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        const char *subject = "hello world";
        int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject), 0, 0, md, NULL);
        t_check(rc > 0, "pcre2_match() finds \"world\" inside \"hello world\"");

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
        t_check(ovector[0] == 6 && ovector[1] == 11, "match offsets are exactly [6,11)");

        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }

    t_begin("pcre2_match(): a real \"no match\" case (PCRE2_ERROR_NOMATCH)");
    {
        int errorcode;
        PCRE2_SIZE erroroffset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)"xyz", PCRE2_ZERO_TERMINATED, 0,
                                        &errorcode, &erroroffset, NULL);
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        const char *subject = "hello world";
        int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject), 0, 0, md, NULL);
        t_check(rc == PCRE2_ERROR_NOMATCH, "pcre2_match() correctly reports no match");
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }

    t_begin("pcre2_compile() + pcre2_match(): a capture group");
    {
        int errorcode;
        PCRE2_SIZE erroroffset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)"([a-z]+)@([a-z]+)\\.com",
                                        PCRE2_ZERO_TERMINATED, 0, &errorcode, &erroroffset, NULL);
        t_check(re != NULL, "pcre2_compile() succeeds for a pattern with capture groups");

        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        const char *subject = "contact: user@example.com today";
        int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject), 0, 0, md, NULL);
        t_check(rc == 3, "pcre2_match() reports 3 (whole match + 2 capture groups)");

        PCRE2_SIZE len1 = 0;
        PCRE2_UCHAR buf1[32];
        len1 = sizeof(buf1);
        pcre2_substring_copy_bynumber(md, 1, buf1, &len1);
        t_check(strncmp((char *)buf1, "user", len1) == 0 && len1 == 4, "capture group 1 == \"user\"");

        PCRE2_SIZE len2 = sizeof(buf1);
        PCRE2_UCHAR buf2[32];
        len2 = sizeof(buf2);
        pcre2_substring_copy_bynumber(md, 2, buf2, &len2);
        t_check(strncmp((char *)buf2, "example", len2) == 0 && len2 == 7, "capture group 2 == \"example\"");

        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }

    t_begin("pcre2_compile(): a real compile error for an invalid pattern");
    {
        int errorcode;
        PCRE2_SIZE erroroffset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)"(unclosed", PCRE2_ZERO_TERMINATED, 0,
                                        &errorcode, &erroroffset, NULL);
        t_check(re == NULL, "pcre2_compile() rejects an unbalanced-paren pattern");
        if (re) {
            pcre2_code_free(re);
        }
    }

    printf("\npcre2test_pu: %d/%d passed", g_pass, g_pass + g_fail);
    if (g_fail) {
        printf(", %d FAILED\n", g_fail);
    } else {
        printf("\n");
    }
    return g_fail == 0 ? 0 : 1;
}
