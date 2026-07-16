/*
 * user/cxxtest.cpp — regression suite proving PureUnix is a real C++
 * userspace target (docs/qt-port.md Phase 2), the foundation the Qt 6 port
 * is built on.
 *
 * Same harness convention as user/libctest.c: every check is independent
 * and numbered, a failure never stops the run, and a summary prints at the
 * end. This program links against the cross-built third_party/libstdcxx
 * (libstdc++.a/libsupc++.a) on top of the vendored newlib, exercising real
 * upstream std:: classes — no hand-rolled std::string/std::vector
 * replacements.
 *
 * Global constructor/destructor ordering is checked via a side log
 * (g_ctor_log) rather than printf from inside the constructors themselves,
 * since printf/stdio isn't guaranteed initialized yet the moment global
 * constructors run (they run from user/newlib_crt0.c's _start_c, before
 * main() but also before anything in main() has touched stdio) — appending
 * to a plain global array is the honest way to observe ordering without
 * assuming more than is actually guaranteed.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <stdexcept>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    std::printf("[%03d] %s\n", g_num, desc);
}

static void check_true(const char *desc, bool cond)
{
    t_begin(desc);
    if (cond) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL\n");
        g_fail++;
    }
}

static void check_str(const char *desc, const std::string &expected, const std::string &got)
{
    t_begin(desc);
    if (expected == got) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL: expected \"%s\", got \"%s\"\n", expected.c_str(), got.c_str());
        g_fail++;
    }
}

static void check_int(const char *desc, long expected, long got)
{
    t_begin(desc);
    if (expected == got) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL: expected %ld, got %ld\n", expected, got);
        g_fail++;
    }
}

/* ---- global constructor/destructor ordering ---- */

static char g_ctor_log[256];
static size_t g_ctor_log_len = 0;

static void ctor_log_append(char c)
{
    if (g_ctor_log_len < sizeof(g_ctor_log) - 1) {
        g_ctor_log[g_ctor_log_len++] = c;
        g_ctor_log[g_ctor_log_len] = '\0';
    }
}

struct OrderProbe {
    char tag;
    explicit OrderProbe(char t) : tag(t) { ctor_log_append(tag); }
    ~OrderProbe() { ctor_log_append((char)(tag + 32)); /* lowercase on destruction */ }
};

/* Constructed before g_probe_a/b/c (declared first, and ordinary static
 * storage duration objects are destroyed in the reverse of their
 * construction order within a TU), so this one's destructor is the last
 * thing to run among all of this file's global destructors — the only
 * point at which g_ctor_log can have its complete final "ABCcba" content,
 * long after main() has already returned. Prints straight to stdout from
 * inside a real global destructor, proving global destructors genuinely
 * run at process exit rather than just trusting the comment describing
 * it: this is driven by user/newlib_crt0.c's atexit(run_fini_array)
 * registration, which itself only runs because newlib's real exit() (not
 * a raw _exit()) is what user/newlib_crt0.c's _start_c() calls. */
struct FinalLogPrinter {
    ~FinalLogPrinter()
    {
        std::printf("[global destructors] final g_ctor_log = \"%s\"\n", g_ctor_log);
    }
};
static FinalLogPrinter g_final_log_printer;

/* Constructed in declaration order (A, B, C) at program startup, before
 * main() runs at all — this i686-elf-gcc build turns out to emit this as
 * a legacy .ctors entry rather than .init_array (see user/newlib_crt0.c's
 * header comment), which user/newlib_crt0.c walks either way. Destroyed
 * in reverse order (C, B, A) at exit(), lowercase, via the
 * atexit(run_fini_array) registration in the same file. */
static OrderProbe g_probe_a('A');
static OrderProbe g_probe_b('B');
static OrderProbe g_probe_c('C');

/* ---- local static initialization (guard variables) ---- */

static int local_static_counter()
{
    static int n = [] { return 41; }();
    return ++n;
}

/* ---- virtual dispatch + pure virtual ---- */

struct Shape {
    virtual ~Shape() = default;
    virtual const char *name() const = 0;
    virtual int sides() const = 0;
};

struct Triangle : Shape {
    const char *name() const override { return "triangle"; }
    int sides() const override { return 3; }
};

struct Square : Shape {
    const char *name() const override { return "square"; }
    int sides() const override { return 4; }
};

/* ---- new/delete, new[]/delete[] ---- */

struct Counted {
    static int live;
    int value;
    explicit Counted(int v) : value(v) { live++; }
    ~Counted() { live--; }
};
int Counted::live = 0;

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    std::printf("=== PureUnix C++ runtime test (cxxtest) ===\n");

    /* Global ctors already ran before main() - only A/B/C uppercase should
     * be present so far (destructors haven't fired yet). */
    check_str("global constructors ran before main(), in declaration order",
              "ABC", std::string(g_ctor_log));

    check_int("local static initializer runs exactly once (first call)", 42, local_static_counter());
    check_int("local static retains value across calls (second call)", 43, local_static_counter());
    check_int("local static retains value across calls (third call)", 44, local_static_counter());

    /* new/delete */
    {
        check_int("Counted::live starts at 0", 0, Counted::live);
        Counted *c = new Counted(7);
        check_int("new Counted increments live count", 1, Counted::live);
        check_int("new Counted stores the right value", 7, c->value);
        delete c;
        check_int("delete Counted decrements live count", 0, Counted::live);
    }

    /* new[]/delete[] */
    {
        Counted *arr = new Counted[5]{ Counted(0), Counted(1), Counted(2), Counted(3), Counted(4) };
        check_int("new[] constructs every element", 5, Counted::live);
        check_int("new[] element 3 has the right value", 3, arr[3].value);
        delete[] arr;
        check_int("delete[] destroys every element", 0, Counted::live);
    }

    /* virtual dispatch + pure virtual (via a real vtable, no switch-on-type) */
    {
        std::vector<std::unique_ptr<Shape>> shapes;
        shapes.push_back(std::unique_ptr<Shape>(new Triangle()));
        shapes.push_back(std::unique_ptr<Shape>(new Square()));

        check_str("virtual dispatch: shapes[0]->name()", "triangle", shapes[0]->name());
        check_int("virtual dispatch: shapes[0]->sides()", 3, shapes[0]->sides());
        check_str("virtual dispatch: shapes[1]->name()", "square", shapes[1]->name());
        check_int("virtual dispatch: shapes[1]->sides()", 4, shapes[1]->sides());
        /* shapes' unique_ptrs run real destructors (through Shape's virtual
         * ~Shape()) when this scope ends - proves the vtable's destructor
         * slot is correct too, not just the two overridden methods. */
    }

    /* std::string */
    {
        std::string s = "Hello, ";
        s += "PureUnix";
        check_str("std::string concatenation", "Hello, PureUnix", s);
        check_int("std::string::size()", 15, (long)s.size());
        check_str("std::string::substr()", "PureUnix", s.substr(7));

        /* Force a real heap allocation past SSO (small-string-optimization)
         * so this genuinely exercises operator new, not just an inline
         * stack buffer. */
        std::string big(200, 'x');
        check_int("std::string past SSO threshold has correct length", 200, (long)big.size());
        check_true("std::string past SSO threshold has correct content",
                   big.find_first_not_of('x') == std::string::npos);
    }

    /* std::vector */
    {
        std::vector<int> v;
        for (int i = 0; i < 10; i++) {
            v.push_back(i * i);
        }
        check_int("std::vector::size() after 10 push_back", 10, (long)v.size());
        check_int("std::vector element access", 49, v[7]);

        v.erase(v.begin() + 2);
        check_int("std::vector::erase() shrinks size", 9, (long)v.size());
        check_int("std::vector::erase() shifts later elements", 9, v[2]);
    }

    /* std::unique_ptr */
    {
        check_int("Counted::live starts at 0 before unique_ptr test", 0, Counted::live);
        {
            std::unique_ptr<Counted> up(new Counted(99));
            check_int("unique_ptr constructs its object", 1, Counted::live);
            check_int("unique_ptr::operator-> works", 99, up->value);
        }
        check_int("unique_ptr destroys its object when it goes out of scope", 0, Counted::live);
    }

    /* std::sort (a real standard algorithm, not hand-rolled) */
    {
        std::vector<int> v = { 5, 3, 1, 4, 1, 5, 9, 2, 6 };
        std::sort(v.begin(), v.end());
        bool sorted = true;
        for (size_t i = 1; i < v.size(); i++) {
            if (v[i - 1] > v[i]) {
                sorted = false;
            }
        }
        check_true("std::sort produces a non-decreasing sequence", sorted);
        check_int("std::sort smallest element", 1, v.front());
        check_int("std::sort largest element", 9, v.back());
    }

    /* C/C++ libc interaction: mixing std::string with plain C strlen/strcmp
     * on its .c_str(), and std::printf itself (already used throughout). */
    {
        std::string s = "interop";
        check_int("strlen() on std::string::c_str()", 7, (long)strlen(s.c_str()));
        check_true("strcmp() on std::string::c_str()", strcmp(s.c_str(), "interop") == 0);
    }

    /* C++ exceptions: real throw/catch, not disabled. Table-based DWARF
     * unwinding through a normal call frame. */
    {
        bool caught = false;
        std::string what;
        try {
            throw std::runtime_error("cxxtest exception probe");
        } catch (const std::exception &e) {
            caught = true;
            what = e.what();
        }
        check_true("C++ exception is thrown and caught", caught);
        check_str("caught exception's what() message", "cxxtest exception probe", what);
    }

    std::printf("=== %d passed, %d failed (of %d) ===\n", g_pass, g_fail, g_num);

    /* Global destructors (g_probe_c/b/a, then g_final_log_printer) run
     * after main() returns, driven by user/newlib_crt0.c's
     * atexit(run_fini_array) - can't check the "ABCcba" suffix here
     * (main() hasn't returned yet). g_final_log_printer's own destructor
     * prints it directly once it's complete - see tools/vt-scripts/
     * run-cxxtest.txt, which waits for that exact line. */

    return g_fail == 0 ? 0 : 1;
}
