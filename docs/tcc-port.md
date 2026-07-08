# TinyCC Port

## Overview

[TinyCC](https://bellard.org/tcc/) (TCC) 0.9.27 runs natively inside
PureUNIX as `/bin/tcc` — a real, ring-3 PureUNIX ELF process that itself
compiles and links C source into other PureUNIX ELF executables:

```
$ tcc hello.c
$ ./a.out
hello from tcc
```

`tcc -c`, multi-file compilation (`tcc a.c b.c`), and `-E` preprocessing all
work the same way any other TCC install would be used. TCC is vendored as
source under `third_party/tcc/` (see that directory's `README.md` for the
vendoring rationale and the exact patch list) and built directly by the
top-level `Makefile` — it is not a separate "port" script, just another
entry in the same build that already produces the kernel and every other
user program.

This document covers the parts specific to *this* port: the sysroot layout,
every incompatibility found while bringing it up, and what's still missing.
For the patches themselves, see `third_party/tcc/README.md`, which is the
canonical, upstream-facing description of what changed and why.

---

## Architecture

### Two translation units, no upstream build system

TCC's own `./configure`/`Makefile`/`lib/Makefile` are not used at all (see
`third_party/tcc/README.md`'s "Why not TCC's own build system"). Instead,
the top-level `Makefile` compiles:

- `libtcc.c` — the amalgamated compiler core. TCC's own `ONE_SOURCE=1`
  default (`tcc.h`) makes this file `#include` `tccpp.c`, `tccgen.c`,
  `tccelf.c`, `tccasm.c`, `tccrun.c`, `i386-gen.c`, `i386-link.c`, and
  `i386-asm.c` directly, so `libtcc.c` alone is the entire compiler+linker.
- `tcc.c` — the CLI frontend, compiled with `-DONE_SOURCE=0` so it does
  *not* re-include `libtcc.c` a second time; it links against libtcc.c's
  public `libtcc.h` API instead. (`tcc.c` also always `#include`s
  `tcctools.c` regardless of `ONE_SOURCE`.)

Both are linked together with `newlib_crt0`/`newlib_syscalls.o` exactly
like any other `NEWLIB_PROGRAMS` entry (`docs/userland.md`) into
`build/user/tcc.elf`.

### The `-D` flags standing in for `./configure`

TCC's own `./configure` would normally generate a `config.h` plus a set of
`-D` flags describing install paths (`CONFIG_TCC_CRTPREFIX`,
`CONFIG_TCC_SYSINCLUDEPATHS`, `CONFIG_TCC_LIBPATHS`, `CONFIG_TCCDIR`) and
target (`TCC_TARGET_I386`). The Makefile's `TCC_DEFINES` supplies these
directly:

| Define | Value | Purpose |
|---|---|---|
| `TCC_TARGET_I386` | (defined) | i386 code generator |
| `TCC_PUREUNIX` | (defined) | Gates the PureUNIX-specific patches — see `third_party/tcc/README.md` |
| `CONFIG_TCC_STATIC` | (defined) | Compiles in dummy `dlopen`/`dlsym`/... (`tccrun.c`) instead of requiring real `libdl`, which PureUNIX doesn't have |
| `CONFIG_TCCDIR` | `/lib/tcc` | Base dir `TCC_LIBTCC1`'s default filename (`libtcc1.a`) resolves against |
| `CONFIG_TCC_CRTPREFIX` | `/lib/tcc/lib` | Where `crt1.o`/`crti.o`/`crtn.o` are found |
| `CONFIG_TCC_SYSINCLUDEPATHS` | `/lib/tcc/include:/usr/include/pureunix-compat:/usr/include` | Ordered header search — see below |
| `CONFIG_TCC_LIBPATHS` | `/lib/tcc/lib:/usr/lib` | Where `-lc`/`-lm`/`-ltcc1` (and any `-l...`) are searched |

### The target sysroot

Assembled by the Makefile's `tcc-sysroot` target into `build/tcc-sysroot/`
and installed onto the EXT2 image by `tools/mkext2.py`'s `add_tcc()`:

| On-disk path | Contents | Why there |
|---|---|---|
| `/lib/tcc/include` | TCC's own 5 compiler-intrinsic headers (`float.h`, `stddef.h`, `stdbool.h`, `stdarg.h`, `varargs.h`) | First in `CONFIG_TCC_SYSINCLUDEPATHS` |
| `/usr/include/pureunix-compat` | A copy of `user/newlib_compat/` | Second — shadows newlib's own headers exactly like `NEWLIB_CFLAGS`'s `-isystem` ordering does at PureUNIX's own build time (`docs/userland.md`) |
| `/usr/include` | A copy of `third_party/newlib/i686-elf/include` | Last — the real newlib header set |
| `/lib/tcc/lib/crt1.o` | Reused `newlib_crt0.S`/`.c`, partial-linked (`ld -r`) into one object | TCC hard-requires the name `crt1.o` (`tccelf.c`) |
| `/lib/tcc/lib/crti.o`, `crtn.o` | New, trivial (`user/tcc_crti.S`/`user/tcc_crtn.S`) | Open/close the (empty) `_init`/`_fini` bodies TCC's linker expects to exist — no `.init_array` support (see Gaps) |
| `/lib/tcc/libtcc1.a` | TCC's runtime helpers (64-bit divide/shift, float↔int conversions), built directly with `i686-elf-gcc` (`lib/libtcc1.c` + `lib/alloca86.S`/`alloca86-bt.S`), no bootstrap `tcc` needed | **Not** under `/lib/tcc/lib/` — see "libtcc1.a's path convention" below |
| `/usr/lib/libc.a` | newlib's vendored `libc.a` with `newlib_syscalls.o`'s POSIX names appended (`ar r`) | So TCC's own unmodified default `-lc` resolves `open`/`read`/`fork`/... |
| `/usr/lib/libm.a` | newlib's vendored `libm.a`, unchanged | `-lm` |
| `/bin/tcc` | `tcc.elf` | The compiler itself |

---

## Incompatibilities found (and fixed)

Everything below was found by actually running `tcc` inside PureUNIX under
QEMU and iterating — not inferred from reading source. Each was root-caused
and fixed at the layer the task's own instructions call for: TCC source
only for the two documented `TCC_PUREUNIX` patches, PureUNIX's libc/syscall
layer for everything else.

### 1. `tcc -v` failed with "memory full (malloc)" before reading any source

**Symptom**: every single `tcc` invocation, including a bare `tcc -v`,
failed immediately with `tcc: error: memory full (malloc)`.

**Root cause**: `tccpp_new()` (`tccpp.c`) unconditionally reserves three
fixed "tiny allocator" pools at startup — `TOKSYM_TAL_SIZE` (768 KiB),
`TOKSTR_TAL_SIZE` (768 KiB), `CSTR_TAL_SIZE` (256 KiB) — 1.8 MiB total,
*before compiling a single byte*. PureUNIX's newlib heap
(`user/newlib_syscalls.c`'s `NEWLIB_HEAP_SIZE`) was 1 MiB. `sizeof(TCCState)`
itself is only 936 bytes on i386 (confirmed by compiling a probe with the
real cross toolchain) — the failure had nothing to do with genuine
compilation memory pressure, purely these three fixed reservations.

**Fix**: `third_party/tcc/tcc-0.9.27/tccpp.c` — under `TCC_PUREUNIX`, the
three pool sizes shrink to 64 KiB / 64 KiB / 32 KiB (160 KiB total). TCC's
own `tal_realloc_impl()` falls back to plain `tcc_malloc()`/`tcc_realloc()`
once a pool is exhausted, so this is a pure performance tradeoff (a few
more real `malloc()` calls under heavy load), never a correctness change.
See `third_party/tcc/README.md`'s patch list.

### 2. `mprotect` was an implicit-declaration compile error

**Symptom**: `libtcc.c` failed to compile — `tccrun.c`'s
`set_pages_executable()` (used by `-run`) calls `mprotect()`, which
PureUNIX had never needed before (no `-run`-style JIT execution existed
anywhere in the userland).

**Fix**: `user/newlib_syscalls.c` gained a real `mprotect()` — not a stub.
PureUNIX's VMM never marks a user page non-writable or non-executable at
all (`kernel/elf.c` maps every user page `PAGE_USER|PAGE_WRITE`, no NX-bit
support — see `docs/memory.md`), so *every* byte a process can address is
already simultaneously read/write/exec. Returning success without touching
anything is an accurate description of this kernel's actual (weak) memory
model, not a shortcut. Declared in `user/newlib_compat/sys/mman.h`.

### 3. `tcc hello.c` linked but produced `libtcc1.a` not found at the wrong path

**Symptom**: after fix #1, `tcc hello.c` got as far as the final link step
and failed: `tcc: error: file '/lib/tcc/libtcc1.a' not found` — even though
`libtcc1.a` was genuinely installed, just one directory level off.

**Root cause**: `TCC_LIBTCC1` (`tcc.h`) defaults to the bare filename
`"libtcc1.a"`, resolved directly against `CONFIG_TCCDIR` (`/lib/tcc`) — a
*different* convention from `CONFIG_TCC_CRTPREFIX`/`CONFIG_TCC_LIBPATHS`,
which both point at `/lib/tcc/lib`. Upstream's own Makefile only overrides
`TCC_LIBTCC1` to include a `lib/` prefix in the cross-compiler case
(`DEF-unx += -DTCC_LIBTCC1="\"lib/$(X)libtcc1.a\""`), which this port never
triggers.

**Fix**: `tools/mkext2.py`'s `add_tcc()` installs `libtcc1.a` directly at
`/lib/tcc/libtcc1.a` (matching the default, unmodified `TCC_LIBTCC1`) while
`crt1.o`/`crti.o`/`crtn.o` stay at `/lib/tcc/lib/` (`CONFIG_TCC_CRTPREFIX`).
No TCC-side patch needed — this was purely an install-layout mismatch.

### 4. Compiled executables couldn't run: `Permission denied`

**Symptom**: after fix #3, `tcc hello.c` completed with no errors, but
`./a.out` failed: `/tctests/a.out: permission denied`.

**Root cause**: TCC's `tcc_write_elf_file()` (`tccelf.c`) creates its
output via `open(filename, O_WRONLY|O_CREAT|O_TRUNC, mode)` with
`mode = 0777` for an executable — correct POSIX usage. But PureUNIX's raw
`SYS_OPEN` syscall (`docs/syscalls.md`) has **no mode argument at all**,
just `path` and `flags` — every file `vfs_create()` makes gets a fixed
default mode regardless of what a POSIX caller requests. `elf_exec()`
(`kernel/elf.c`) requires `X_OK`, so a freshly-compiled executable could
never actually run.

**Fix**: `user/newlib_syscalls.c`'s `open()` now reads the variadic `mode`
argument (previously silently dropped) and, when the file is genuinely
*created* by this call (checked via `access(path, F_OK)` beforehand, so an
`O_CREAT` open of an already-existing file — e.g. `touch` on an existing
file — doesn't clobber its permissions), issues a follow-up `chmod(path,
mode)` using the already-real `SYS_CHMOD`. Slightly non-atomic (the file
briefly exists with `vfs_create()`'s own default mode) but PureUNIX has no
concurrent-access model where that's observable — the same kind of
best-effort translation this file already uses for `mmap()`/`mprotect()`.
This fixes file creation permissions for *every* newlib-linked program
(`open(..., O_CREAT, mode)` with a non-default mode), not just TCC.

### 5. EXT2 image outgrew its old 4 MiB size

**Symptom**: not a runtime failure — the sysroot (newlib's ~140 headers,
`libc.a`/`libm.a`, TCC's own compiler intrinsics) plus `tcc.elf` itself
(~380 KiB) didn't fit in the old 4 MiB EXT2 image alongside everything
already installed.

**Fix**: `tools/mkext2.py`'s `TOTAL_BLOCKS` doubled to 8192 (8 MiB) — still
within the single-block-group ceiling this builder's 1 KiB block size
imposes (`BLOCKS_PER_GROUP = 8192`, the max one block-bitmap block can
cover), so no structural change to the builder or the kernel's EXT2 driver
was needed, just the size constant.

---

## Memory budget

The per-process address window is fixed at `USER_WINDOW_BASE=0x08000000`
to `USER_WINDOW_END=0x08300000` (`include/pureunix/vmm.h`) — 3 MiB total,
minus a 64 KiB stack, for code+data+heap combined; architecturally capped
at 4 MiB total by the current single-page-table-entry (PDE 32) design
(`docs/memory.md`). `tcc.elf` itself uses ~350 KiB text + ~1.13 MiB
data/bss (almost entirely the 1 MiB newlib heap array), leaving comfortable
headroom for the small-to-medium programs this port's test suite compiles
(see Testing below). Fix #1 above (shrinking TCC's own fixed tal-pool
reservation from 1.8 MiB to 160 KiB) was necessary to leave *any* usable
heap at all within that budget.

---

## Testing

`/tcctests/` on the EXT2 image (installed by `tools/mkext2.py`, permanent
regression fixtures alongside `bigfile.bin`/`hugefile.bin`/`perm/`) holds
small C programs exercising the ladder this port was brought up against:

- `hello.c` — `tcc hello.c && ./a.out` (the primary success criterion)
- `cat.c` — `open`/`read`/`write` syscalls directly
- `grep.c` — `fopen`/`fgets`/`strstr` (stdio-based)
- `mainc.c` + `helper.c` + `helper.h` — multi-file compilation and linking
  (`tcc mainc.c helper.c`)
- `greeting.txt` — fixture data for `cat.c`/`grep.c`

All verified working end-to-end under QEMU (`make run`, real PS/2 keyboard
input via QEMU's HMP `sendkey` — PureUNIX's console only reads from the
PS/2 keyboard, never the serial port, so a purely serial-driven headless
test harness can't type into it): `tcc -v`, `tcc hello.c && ./a.out`
(prints `hello from tcc`), `tcc -c hello.c` (produces `hello.o`), `tcc
mainc.c helper.c && ./a.out` (prints `sum is 42`, i.e. `19+23`), `tcc -E
hello.c` (correct preprocessed output to stdout), `tcc -o catprog cat.c &&
./catprog greeting.txt`, `tcc -o grepprog grep.c && ./grepprog needle
greeting.txt`.

### Self-hosting (stretch goal)

Attempted, inconclusively. TCC's own amalgamated source (`tcc.c`, which
`#include`s `libtcc.c`/`tccpp.c`/`tccgen.c`/`tccelf.c`/`tccasm.c`/
`tccrun.c`/`i386-{gen,link,asm}.c`/`tcctools.c` — ~930 KiB of source) was
staged on a test disk image for the native `tcc.elf` to compile with the
same `TCC_DEFINES` the Makefile uses. The QEMU test harness used for every
other result in this document (real PS/2 keystroke injection via the HMP
monitor's `sendkey` — see "Testing" above) proved unreliable for this
longer, more complex interactive sequence: entire typed command lines
occasionally produced no visible effect at all (not even an echoed
prompt), a failure mode not seen for the shorter command sequences the
rest of this port's testing used. One run did produce a shell error
(`sh: 3: m`) immediately after invoking a wrapper script, but without a
clean, repeatable capture it isn't possible to tell whether that reflects
a real self-hosting problem or is itself a symptom of the same dropped-
keystroke issue (the wrapper script's own content was verified byte-for-
byte correct and syntactically valid against a reference POSIX shell
before staging it).

**What's genuinely unresolved**: the memory-budget concern this document's
"Memory budget" section already flagged — TCC compiling its own ~930 KiB,
much larger than any of the verified-working test programs, is a real
open question, not yet confirmed to work *or* fail. Retrying this with a
more robust test harness (or a kernel-side scripted-input mechanism that
doesn't depend on emulated keystroke timing) is the natural next step
before concluding either way.

---

## Remaining gaps

- **No `-run` verification.** `tccrun.c`'s JIT-execution path compiles
  (`TCC_IS_NATIVE` is set, since host arch == target arch == i386) and
  might well work by accident — PureUNIX's `mmap()` is a `malloc()`
  wrapper and every page is already executable (no NX bit, see fix #2
  above) — but this was never exercised. Not a stated success criterion.
- **No dynamic linking.** Forced static (`TCC_PUREUNIX` defaults
  `s->static_link = 1`, see `third_party/tcc/README.md`) — PureUNIX has no
  dynamic linker at all.
- **No `.init_array`/`.fini_array` constructor support.** `crti.o`/`crtn.o`
  (`user/tcc_crti.S`/`user/tcc_crtn.S`) produce empty, never-called
  `_init`/`_fini` functions purely to satisfy the linker's structural
  expectation. C99 doesn't need this; C++-style global constructors would.
- **No bounds checker (`-b`).** `CONFIG_TCC_BCHECK` is excluded under
  `TCC_PUREUNIX` alongside `CONFIG_TCC_BACKTRACE` (both would need a real
  `SIGSEGV`-delivery mechanism PureUNIX doesn't have — see
  `third_party/tcc/README.md`'s patch list). `bcheck.o` is never built.
- **Static library archives** (`tcc -ar`, or linking against a prebuilt
  `.a`) were not exercised by the test suite above, though nothing in the
  incompatibilities found suggests they wouldn't work — `i686-elf-ar` and
  TCC's own `-ar` tool mode both operate on plain `ar` archives the same
  way regardless of target.
