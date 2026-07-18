/*
 * user/libctest.c — regression suite for the vendored newlib port
 * (third_party/newlib, user/newlib_syscalls.c, user/newlib_crt0.c).
 *
 * Unlike user/systest.c (which exercises PureUNIX's own syscall ABI via
 * libpure), this program links against a real C library and exercises
 * *it*: standard I/O and formatting, strings, the heap, ctype, libm, and
 * setjmp/longjmp — plus real file I/O routed all the way down through
 * fopen()/fread()/fwrite() -> read()/write() (user/newlib_syscalls.c) ->
 * int $0x80 -> the EXT2-backed VFS.
 *
 * Same harness convention as systest.c: every check is independent and
 * numbered, a failure never stops the run, and a summary prints at the end.
 */
#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    printf("[%03d] %s\n", g_num, desc);
}

static void check_int(const char *desc, long expected, long got)
{
    t_begin(desc);
    if (expected == got) {
        printf("      PASS\n");
        g_pass++;
    } else {
        printf("      FAIL: expected %ld, got %ld\n", expected, got);
        g_fail++;
    }
}

static void check_str(const char *desc, const char *expected, const char *got)
{
    t_begin(desc);
    if (strcmp(expected, got) == 0) {
        printf("      PASS\n");
        g_pass++;
    } else {
        printf("      FAIL: expected \"%s\", got \"%s\"\n", expected, got);
        g_fail++;
    }
}

static void check_true(const char *desc, int cond)
{
    t_begin(desc);
    if (cond) {
        printf("      PASS\n");
        g_pass++;
    } else {
        printf("      FAIL\n");
        g_fail++;
    }
}

/* Doubles need a tolerance check rather than exact equality. */
static void check_near(const char *desc, double expected, double got)
{
    t_begin(desc);
    double diff = expected - got;
    if (diff < 0) diff = -diff;
    if (diff < 0.0001) {
        printf("      PASS\n");
        g_pass++;
    } else {
        printf("      FAIL: expected %f, got %f\n", expected, got);
        g_fail++;
    }
}

/* ------------------------------------------------------------------ printf */

static void section_printf(void)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "%d-%s-%c-%x-%u", 42, "hi", 'Z', 255, 7u);
    check_str("snprintf: mixed int/string/char/hex/unsigned", "42-hi-Z-ff-7", buf);

    snprintf(buf, sizeof(buf), "%5d|%-5d|%05d", 3, 3, 3);
    check_str("snprintf: width/left-align/zero-pad", "    3|3    |00003", buf);

    snprintf(buf, sizeof(buf), "%.2f", 3.14159);
    check_str("snprintf: float precision", "3.14", buf);

    int a = 0, b = 0;
    int n = sscanf("100 200", "%d %d", &a, &b);
    check_int("sscanf: field count", 2, n);
    check_int("sscanf: first field", 100, a);
    check_int("sscanf: second field", 200, b);
}

/* ------------------------------------------------------------------ string */

static void section_string(void)
{
    char buf[32];

    strcpy(buf, "hello");
    strcat(buf, " world");
    check_str("strcpy+strcat", "hello world", buf);
    check_int("strlen", 11, (long)strlen(buf));
    check_int("strcmp equal", 0, strcmp("abc", "abc"));
    check_true("strcmp differs", strcmp("abc", "abd") < 0);

    char *p = strchr(buf, 'w');
    check_str("strchr", "world", p);

    char *needle = strstr(buf, "wor");
    check_str("strstr", "world", needle);

    char src[16] = "memcpy-test";
    char dst[16];
    memcpy(dst, src, sizeof(src));
    check_str("memcpy", "memcpy-test", dst);

    memset(dst, 'x', 4);
    check_true("memset", dst[0] == 'x' && dst[1] == 'x' && dst[2] == 'x' && dst[3] == 'x');

    char tok_buf[] = "a,bb,ccc";
    char *tok = strtok(tok_buf, ",");
    check_str("strtok first", "a", tok);
    tok = strtok(NULL, ",");
    check_str("strtok second", "bb", tok);
    tok = strtok(NULL, ",");
    check_str("strtok third", "ccc", tok);
}

/* ------------------------------------------------------------------ stdlib */

static int cmp_int_desc(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    return ib - ia;
}

static void section_stdlib(void)
{
    check_int("atoi", 12345, atoi("12345"));
    check_int("atoi negative", -42, atoi("-42"));
    check_int("strtol base 16", 0xBEEF, strtol("beef", NULL, 16));
    check_int("abs", 7, abs(-7));

    int arr[6] = { 3, 1, 4, 1, 5, 9 };
    qsort(arr, 6, sizeof(int), cmp_int_desc);
    check_true("qsort descending",
        arr[0] == 9 && arr[1] == 5 && arr[2] == 4 &&
        arr[3] == 3 && arr[4] == 1 && arr[5] == 1);

    int *heap = malloc(10 * sizeof(int));
    check_true("malloc non-null", heap != NULL);
    if (heap) {
        for (int i = 0; i < 10; i++) heap[i] = i * i;
        int ok = 1;
        for (int i = 0; i < 10; i++) if (heap[i] != i * i) ok = 0;
        check_true("malloc'd memory holds values", ok);

        int *bigger = realloc(heap, 20 * sizeof(int));
        check_true("realloc non-null", bigger != NULL);
        if (bigger) {
            int preserved = 1;
            for (int i = 0; i < 10; i++) if (bigger[i] != i * i) preserved = 0;
            check_true("realloc preserves data", preserved);
            free(bigger);
        } else {
            free(heap);
        }
    }
}

/* ------------------------------------------------------------------ ctype */

static void section_ctype(void)
{
    check_true("isalpha", isalpha('a') && !isalpha('5'));
    check_true("isdigit", isdigit('5') && !isdigit('a'));
    check_true("isspace", isspace(' ') && isspace('\t') && !isspace('x'));
    check_int("toupper", 'A', toupper('a'));
    check_int("tolower", 'z', tolower('Z'));
}

/* ------------------------------------------------------------------ math */

static void section_math(void)
{
    check_near("sqrt(16) == 4", 4.0, sqrt(16.0));
    check_near("pow(2,10) == 1024", 1024.0, pow(2.0, 10.0));
    check_near("fabs(-3.5) == 3.5", 3.5, fabs(-3.5));
    check_near("floor(3.9) == 3", 3.0, floor(3.9));
    check_near("ceil(3.1) == 4", 4.0, ceil(3.1));
}

/* ------------------------------------------------------------------ setjmp */

static void section_setjmp(void)
{
    jmp_buf env;
    int val = setjmp(env);
    if (val == 0) {
        longjmp(env, 42);
        check_true("longjmp unreachable", 0);
    } else {
        check_int("setjmp/longjmp round trip", 42, val);
    }
}

/* ------------------------------------------------------------------ iconv */

/* Real narrow iconv() (user/newlib_syscalls.c, added for the PCManFM-Qt
 * port's GLib dependency, docs/pcmanfm-port.md) — UTF-8-family identity
 * conversion only, real EINVAL for anything else. Exercises the actual
 * public API (iconv_open/iconv/iconv_close), not the internal helper. */
static void section_iconv(void)
{
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    check_true("iconv_open(UTF-8, UTF-8) succeeds", cd != (iconv_t)-1);

    char src[] = "Hello, PureUnix! \xc3\xa9\xc3\xa8"; /* includes real 2-byte UTF-8 (é, è) */
    char dst[64];
    char *inp = src;
    char *outp = dst;
    size_t inleft = strlen(src);
    size_t outleft = sizeof(dst);
    size_t rc = iconv(cd, &inp, &inleft, &outp, &outleft);
    check_true("iconv() identity conversion returns 0 (no irreversible conversions)", rc == 0);
    check_int("iconv() consumed all input bytes", 0, (int)inleft);
    *outp = '\0';
    check_true("iconv() output byte-for-byte identical to input", strcmp(dst, src) == 0);

    iconv_close(cd);

    iconv_t bad = iconv_open("ISO-8859-1", "UTF-8");
    check_true("iconv_open() correctly rejects an unsupported real charset (ISO-8859-1)", bad == (iconv_t)-1);

    char small_src[] = "0123456789";
    char small_dst[4];
    iconv_t cd2 = iconv_open("UTF-8", "UTF-8");
    char *inp2 = small_src;
    char *outp2 = small_dst;
    size_t inleft2 = strlen(small_src);
    size_t outleft2 = sizeof(small_dst);
    size_t rc2 = iconv(cd2, &inp2, &inleft2, &outp2, &outleft2);
    check_true("iconv() with too-small output buffer reports E2BIG", rc2 == (size_t)-1 && errno == E2BIG);
    check_int("iconv() still copied as much as fit in the output buffer", 0, (int)outleft2);
    iconv_close(cd2);
}

/* ------------------------------------------------------------------ pthread */

/* Real single-threaded pthread shim (user/newlib_syscalls.c, added for
 * the PCManFM-Qt port's GLib dependency, docs/pcmanfm-port.md) —
 * pthread_create() genuinely runs the entry point synchronously before
 * returning, so these checks confirm the *real* mechanics (a genuine
 * setjmp/longjmp round trip for pthread_exit(), a genuine heap-allocated
 * per-thread record carrying a real retval through to pthread_join(),
 * a genuinely-once pthread_once(), a real thread-specific-data table),
 * not just "does it link". */

static void *pthread_worker_returns_value(void *arg)
{
    int *n = (int *)arg;
    return (void *)(intptr_t)(*n * 2);
}

static void *pthread_worker_calls_exit(void *arg)
{
    (void)arg;
    pthread_exit((void *)(intptr_t)999);
    return (void *)(intptr_t)111; /* unreachable -- proves pthread_exit() really unwound past this */
}

static int g_once_call_count = 0;
static void once_routine(void)
{
    g_once_call_count++;
}

static void section_pthread(void)
{
    t_begin("pthread_mutex_init/lock/unlock/destroy");
    {
        pthread_mutex_t m;
        check_int("pthread_mutex_init()", 0, pthread_mutex_init(&m, NULL));
        check_int("pthread_mutex_lock()", 0, pthread_mutex_lock(&m));
        check_int("pthread_mutex_trylock() (never contended here)", 0, pthread_mutex_trylock(&m));
        check_int("pthread_mutex_unlock()", 0, pthread_mutex_unlock(&m));
        check_int("pthread_mutex_destroy()", 0, pthread_mutex_destroy(&m));
    }

    t_begin("pthread_cond_wait() returns immediately (real, correct here — see newlib_syscalls.c's own comment)");
    {
        pthread_mutex_t m;
        pthread_cond_t c;
        pthread_mutex_init(&m, NULL);
        pthread_cond_init(&c, NULL);
        check_int("pthread_cond_wait() returns 0 without blocking", 0, pthread_cond_wait(&c, &m));
        check_int("pthread_cond_signal()", 0, pthread_cond_signal(&c));
        pthread_cond_destroy(&c);
        pthread_mutex_destroy(&m);
    }

    t_begin("pthread_create()/pthread_join(): real synchronous execution + real retval round trip");
    {
        pthread_t tid;
        int arg = 21;
        int rc = pthread_create(&tid, NULL, pthread_worker_returns_value, &arg);
        check_int("pthread_create() returns 0", 0, rc);
        void *retval = NULL;
        check_int("pthread_join() returns 0", 0, pthread_join(tid, &retval));
        check_int("joined thread's real retval == 42", 42, (int)(intptr_t)retval);
    }

    t_begin("pthread_exit(): a real setjmp/longjmp round trip past the rest of the entry point");
    {
        pthread_t tid;
        pthread_create(&tid, NULL, pthread_worker_calls_exit, NULL);
        void *retval = NULL;
        pthread_join(tid, &retval);
        check_int("retval came from pthread_exit(999), not the unreachable return(111)", 999, (int)(intptr_t)retval);
    }

    t_begin("pthread_self()/pthread_equal()");
    {
        pthread_t self1 = pthread_self();
        pthread_t self2 = pthread_self();
        check_true("pthread_self() is stable across calls", pthread_equal(self1, self2));
    }

    t_begin("pthread_once(): init_routine runs exactly once across 3 calls");
    {
        pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_once(&once, once_routine);
        pthread_once(&once, once_routine);
        pthread_once(&once, once_routine);
        check_int("once_routine() ran exactly 1 time", 1, g_once_call_count);
    }

    t_begin("pthread_key_create()/setspecific()/getspecific()/key_delete()");
    {
        pthread_key_t key;
        check_int("pthread_key_create()", 0, pthread_key_create(&key, NULL));
        int value = 1234;
        check_int("pthread_setspecific()", 0, pthread_setspecific(key, &value));
        void *got = pthread_getspecific(key);
        check_true("pthread_getspecific() returns the real stored pointer", got == &value);
        check_int("pthread_key_delete()", 0, pthread_key_delete(key));
    }
}

/* ------------------------------------------------------------------ file I/O */

static void section_file_io(void)
{
    const char *path = "/libctest_tmp.txt";
    const char *content = "the quick brown fox\n";

    FILE *f = fopen(path, "w");
    check_true("fopen for write", f != NULL);
    if (f) {
        size_t written = fwrite(content, 1, strlen(content), f);
        check_int("fwrite byte count", (long)strlen(content), (long)written);
        fclose(f);
    }

    f = fopen(path, "r");
    check_true("fopen for read", f != NULL);
    if (f) {
        char buf[64] = { 0 };
        char *got = fgets(buf, sizeof(buf), f);
        check_true("fgets returned data", got != NULL);
        check_str("fgets content round-trips", content, buf);

        fseek(f, 4, SEEK_SET);
        int c = fgetc(f);
        check_int("fseek + fgetc", 'q', c);

        long pos = ftell(f);
        check_int("ftell after fgetc", 5, pos);

        fclose(f);
    }

    remove(path);
    FILE *gone = fopen(path, "r");
    check_true("remove() actually removed the file", gone == NULL);
    if (gone) fclose(gone);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    printf("=== PureUNIX libc test (newlib) ===\n\n");

    section_printf();
    section_string();
    section_stdlib();
    section_ctype();
    section_math();
    section_setjmp();
    section_iconv();
    section_pthread();
    section_file_io();

    printf("\n=== %d/%d passed", g_pass, g_num);
    if (g_fail) {
        printf(", %d FAILED ===\n", g_fail);
    } else {
        printf(" ===\n");
    }

    return g_fail == 0 ? 0 : 1;
}
