/*
 * user/libffitest.c — real on-target regression test for the vendored
 * libffi cross-build (third_party/libffi/i686-elf, tools/build-libffi.sh).
 * First new dependency ported for the PCManFM-Qt effort
 * (docs/pcmanfm-port.md) — GObject's generic signal-marshalling machinery
 * links against it directly.
 *
 * Not just "does it link" — this actually drives libffi's real dynamic
 * calling-convention machinery (ffi_prep_cif + ffi_call) to invoke a real
 * C function through it and checks the real result, the same class of
 * genuine on-target check every other vendored dependency in this repo
 * gets (see user/libctest.c/qtcoretest.cpp/cxxtest.cpp).
 *
 * Same harness convention as systest.c/libctest.c: every check is
 * independent and numbered, a failure never stops the run, a summary
 * prints at the end.
 */
#include <ffi.h>
#include <stdio.h>

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

static int add_ints(int a, int b)
{
    return a + b;
}

static double scale_double(double x, int n)
{
    return x * (double)n;
}

int main(void)
{
    t_begin("ffi_prep_cif + ffi_call: int add_ints(int, int)");
    {
        ffi_cif cif;
        ffi_type *arg_types[2] = { &ffi_type_sint, &ffi_type_sint };
        ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint, arg_types);
        t_check(st == FFI_OK, "ffi_prep_cif() returned FFI_OK");

        int a = 3, b = 4;
        void *args[2] = { &a, &b };
        int result = 0;
        ffi_call(&cif, FFI_FN(add_ints), &result, args);
        t_check(result == 7, "ffi_call() computed add_ints(3, 4) == 7");
    }

    t_begin("ffi_prep_cif + ffi_call: double scale_double(double, int)");
    {
        ffi_cif cif;
        ffi_type *arg_types[2] = { &ffi_type_double, &ffi_type_sint };
        ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_double, arg_types);
        t_check(st == FFI_OK, "ffi_prep_cif() returned FFI_OK");

        double x = 2.5;
        int n = 4;
        void *args[2] = { &x, &n };
        double result = 0;
        ffi_call(&cif, FFI_FN(scale_double), &result, args);
        t_check(result == 10.0, "ffi_call() computed scale_double(2.5, 4) == 10.0");
    }

    printf("\nlibffitest: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
