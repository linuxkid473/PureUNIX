CROSS ?= i686-elf-
CC := $(CROSS)gcc
CXX := $(CROSS)g++
AS := $(CROSS)gcc
LD := $(CROSS)gcc
AR := $(CROSS)ar
QEMU ?= qemu-system-i386
PYTHON ?= python3
GRUB_MKRESCUE ?= i686-elf-grub-mkrescue
GRUB_MKIMAGE  ?= i686-elf-grub-mkimage

BUILD := build
KERNEL := $(BUILD)/pureunix.elf
# $(ISO) is now the persistent, USB-flashable hybrid disk image (real MBR +
# GRUB core.img + a writable EXT2 root partition — see tools/mkdiskimg.py)
# that `make iso` is documented to produce: dd it straight onto a USB stick
# and it boots on real hardware as-is, no separate install step. Despite
# the name/extension (kept for a stable, familiar `make iso` -> flash
# workflow — see the task this came from), it is NOT an ISO9660 image.
# $(LIVE_ISO) is the original grub-mkrescue ISO9660-plus-ramdisk-module
# image (kernel + fat.img + root.img loaded straight into RAM, root
# discarded on reboot) — kept, unchanged, purely so run-test's scripted
# regression suite (tools/vt-inject-test.py, 339/341 systest baseline)
# keeps working exactly as it always has; see `run-live` below.
ISO := $(BUILD)/pureunix.iso
LIVE_ISO := $(BUILD)/pureunix-live.iso
DISK  := $(BUILD)/pureunix.img
DISK2 := $(BUILD)/ext2.img
# Persistent-disk-only artifacts (tools/mkdiskimg.py's inputs): a second
# EXT2 image built with --persistent-boot (embeds /boot/pureunix.elf +
# /boot/grub/grub.cfg, and uses the PUREUNIX_ROOT label boot/grub-
# embedded.cfg's `search` looks for), plus a build-time GRUB core.img
# embedding that search script as its prefix config.
DISK_PERSISTENT := $(BUILD)/ext2-persistent.img
CORE_IMG := $(BUILD)/grub/core.img
GRUB_BOOT_IMG := $(BUILD)/grub/boot.img

COMMON_CFLAGS := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Wno-unused-parameter \
	-fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -Iinclude
KERNEL_CFLAGS := $(COMMON_CFLAGS) -D__PUREUNIX_KERNEL__
USER_CFLAGS := $(COMMON_CFLAGS) -fno-builtin
LDFLAGS := -T boot/linker.ld -ffreestanding -O2 -nostdlib -Wl,--build-id=none
USER_LDFLAGS := -T user/linker.ld -ffreestanding -nostdlib -Wl,--build-id=none

# Vendored newlib (see third_party/newlib/README.md and tools/build-newlib.sh)
# — a real C library for user programs, on top of the raw syscall wrappers
# in user/libpure.c/user/newlib_syscalls.c. Only programs in NEWLIB_PROGRAMS
# link against it; everything else keeps using bare libpure.
NEWLIB_DIR := third_party/newlib/i686-elf
# user/newlib_compat/ is searched first so its dirent.h shadows newlib's own
# (third_party/newlib/i686-elf/include/sys/dirent.h is just the generic
# "no host support configured" fallback and #errors out unconditionally —
# see that compat header's comment). Nothing else newlib provides needs
# overriding, so this is a single-file, narrow version of the same
# "compat headers shadow the vendored ones" trick user/vi/compat/ uses.
NEWLIB_CFLAGS := -isystem user/newlib_compat -isystem $(NEWLIB_DIR)/include
NEWLIB_LDFLAGS := $(USER_LDFLAGS) -L$(NEWLIB_DIR)/lib

# Vendored libstdc++-v3 (see third_party/libstdcxx/README.md and
# tools/build-libstdcxx.sh) — cross-built from real upstream GCC 16.1.0
# source against the vendored newlib above, since i686-elf-g++ (Homebrew)
# was built --without-headers and never got a target libstdc++ of its own
# (docs/qt-port.md section 1). libstdc++'s own include dirs must come
# *before* NEWLIB_CFLAGS: headers like <cstdlib> do `#include_next
# <stdlib.h>`, which needs newlib's real stdlib.h to still be later in the
# search path, not already consumed by this same directory.
LIBSTDCXX_DIR := third_party/libstdcxx/i686-elf
LIBSTDCXX_CFLAGS := -isystem $(LIBSTDCXX_DIR)/include/c++/16.1.0 \
	-isystem $(LIBSTDCXX_DIR)/include/c++/16.1.0/i686-elf \
	$(NEWLIB_CFLAGS)
LIBSTDCXX_LDFLAGS := $(NEWLIB_LDFLAGS) -L$(LIBSTDCXX_DIR)/lib
# No -ffreestanding (unlike USER_CFLAGS): it makes GCC predefine
# __STDC_HOSTED__=0, which libstdc++ headers read to decide whether to
# expose the real hosted library at all — PureUnix genuinely is hosted here
# (real newlib libc, real syscalls), see docs/qt-port.md section 1.
USER_CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter \
	-fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -Iinclude

KERNEL_C_SRCS := $(shell find kernel arch drivers fs libc shell editor net -name '*.c' | sort)
KERNEL_AS_SRCS := $(shell find boot arch -name '*.S' | sort)
KERNEL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C_SRCS)) \
	$(patsubst %.S,$(BUILD)/%.o,$(KERNEL_AS_SRCS))
DEPS := $(KERNEL_OBJS:.o=.d)

USER_PROGRAMS := hello calc viewer editor sh opentest readtest ext2test systest termiostest ping font tty
USER_ELFS := $(addprefix $(BUILD)/user/,$(addsuffix .elf,$(USER_PROGRAMS)))

# Neatvi (vi/ex clone, vendored under user/vi/ — see user/vi/vi.h's header
# comment): many .c files compiled into one program, so it needs its own
# object/link rules rather than the single-file %.o/%.elf pattern above.
# user/vi/compat/ supplies the POSIX headers upstream Neatvi expects.
# Named "neatvi" rather than "vi" so it doesn't collide with the shell's
# existing `vi`/`vim` builtins (editor/editor.c, PureUNIX's own small
# in-kernel modal editor) — both are available side by side.
VI_SRCS := $(sort $(wildcard user/vi/*.c))
VI_OBJS := $(patsubst user/vi/%.c,$(BUILD)/user/vi/%.o,$(VI_SRCS))
VI_CFLAGS := $(USER_CFLAGS) -Iuser/vi/compat -Iuser/vi -Iuser

$(BUILD)/user/vi/%.o: user/vi/%.c
	@mkdir -p $(dir $@)
	$(CC) $(VI_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/neatvi.elf: $(VI_OBJS) $(BUILD)/user/crt0.o $(BUILD)/user/libpure.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(BUILD)/user/crt0.o $(BUILD)/user/libpure.o $(VI_OBJS) -lgcc -o $@

USER_PROGRAMS += neatvi
USER_ELFS += $(BUILD)/user/neatvi.elf
DEPS += $(VI_OBJS:.o=.d)

NEWLIB_PROGRAMS := libctest exectest dirtest
NEWLIB_ELFS := $(addprefix $(BUILD)/user/,$(addsuffix .elf,$(NEWLIB_PROGRAMS)))

# regcomp/regexec/regerror/regfree (see third_party/regex/README.md) — the
# POSIX regex engine newlib's own <regex.h> declares but never implements
# for this bare target. Vendored as source (small enough, unlike BusyBox)
# and built here so tools/build-busybox.sh can link the resulting objects
# straight into busybox.elf, giving grep/egrep/fgrep real regex matching.
# engine.c is deliberately NOT in this list: regexec.c #includes it directly
# (three times, with different macros each time — see regexec.c) rather
# than linking it as its own translation unit, exactly like upstream.
REGEX_SRCS := third_party/regex/regcomp.c third_party/regex/regexec.c \
	third_party/regex/regerror.c third_party/regex/regfree.c
REGEX_OBJS := $(patsubst third_party/regex/%.c,$(BUILD)/user/regex/%.o,$(REGEX_SRCS))

$(BUILD)/user/regex/%.o: third_party/regex/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(REGEX_OBJS:.o=.d)

# BusyBox: vendored as a prebuilt ELF (third_party/busybox/busybox.elf,
# built by tools/build-busybox.sh against the same newlib_crt0/
# newlib_syscalls glue as NEWLIB_ELFS above) rather than compiled from
# source on every `make` — see third_party/busybox/README.md. Only goes on
# the EXT2 image: mkext2.py creates a symlink per enabled applet
# (busybox.elf's own docs list which — see BUSYBOX_APPLETS in
# tools/mkext2.py), and FAT16 has no symlinks to hang those off of.
BUSYBOX_ELF := $(BUILD)/user/busybox.elf

$(BUSYBOX_ELF): third_party/busybox/busybox.elf $(REGEX_OBJS)
	@mkdir -p $(dir $@)
	cp $< $@

# TinyCC (vendored source under third_party/tcc/ — see that dir's README.md
# and docs/tcc-port.md for the full design). Built the same way as any other
# NEWLIB_PROGRAMS entry (links against newlib + newlib_crt0 + newlib_syscalls
# — see $(NEWLIB_ELFS) above) except its two translation units (libtcc.c, the
# ONE_SOURCE=1-amalgamated compiler core, and tcc.c, the CLI frontend, built
# with -DONE_SOURCE=0 to *not* re-include libtcc.c a second time — the exact
# split TCC's own upstream Makefile uses) need their own compile rules and a
# set of -D flags standing in for what TCC's own ./configure would generate
# (skipped entirely — see third_party/tcc/README.md's "Why not TCC's own
# build system").
#
# These paths are where the TCC-target sysroot (crt objects, libtcc1.a,
# merged libc.a, headers — assembled below) actually gets installed on the
# EXT2 image (tools/mkext2.py), baked in here as compile-time constants so a
# bare `tcc hello.c` finds everything with no -I/-L/env-var configuration.
TCC_SRC := third_party/tcc/tcc-0.9.27
TCC_SYSROOT_LIB     := /lib/tcc/lib
TCC_SYSROOT_INCLUDE := /lib/tcc/include
TCC_COMPAT_INCLUDE  := /usr/include/pureunix-compat
TCC_USR_INCLUDE     := /usr/include
TCC_USR_LIB         := /usr/lib

TCC_DEFINES := -DTCC_TARGET_I386 -DTCC_PUREUNIX -DCONFIG_TCC_STATIC \
	-DCONFIG_TCCDIR=\"/lib/tcc\" \
	-DCONFIG_TCC_CRTPREFIX=\"$(TCC_SYSROOT_LIB)\" \
	-DCONFIG_TCC_SYSINCLUDEPATHS=\"$(TCC_SYSROOT_INCLUDE):$(TCC_COMPAT_INCLUDE):$(TCC_USR_INCLUDE)\" \
	-DCONFIG_TCC_LIBPATHS=\"$(TCC_SYSROOT_LIB):$(TCC_USR_LIB)\"
TCC_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(TCC_SRC) $(TCC_DEFINES)

$(BUILD)/user/tcc/libtcc.o: $(TCC_SRC)/libtcc.c
	@mkdir -p $(dir $@)
	$(CC) $(TCC_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/tcc/tcc.o: $(TCC_SRC)/tcc.c
	@mkdir -p $(dir $@)
	$(CC) $(TCC_CFLAGS) -DONE_SOURCE=0 -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/tcc/libtcc.d $(BUILD)/user/tcc/tcc.d

TCC_ELF := $(BUILD)/user/tcc.elf

$(TCC_ELF): $(BUILD)/user/tcc/tcc.o $(BUILD)/user/tcc/libtcc.o $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/tcc/tcc.o $(BUILD)/user/tcc/libtcc.o \
		-Wl,--start-group -lc -lm -Wl,--end-group -lgcc -o $@

# ---- TinyCC target sysroot ----
# What TCC-compiled *programs* (not tcc.elf itself) link against at runtime
# inside PureUNIX — crt1.o/crti.o/crtn.o, libtcc1.a (TCC's own runtime
# helpers), and a merged libc.a, plus the header search path. Assembled into
# $(BUILD)/tcc-sysroot/ and installed onto the EXT2 image at the paths
# TCC_DEFINES bakes in above. See docs/tcc-port.md for the full layout.
TCC_SYSROOT := $(BUILD)/tcc-sysroot

# crt1.o: TCC hard-requires this name (tccelf.c) for a plain (non
# -nostdlib) link. Reuses newlib_crt0.S/.c — already exactly a crt1.o
# (reads the kernel's argc/argv/envp frame, calls exit(main(...))) — via a
# partial link (`ld -r`) that merges its two object files (asm entry +
# C body) into the single file TCC's linker expects under that name.
$(TCC_SYSROOT)/lib/crt1.o: $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -Wl,-r -o $@ $^

$(TCC_SYSROOT)/lib/crti.o: user/tcc_crti.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -c $< -o $@

$(TCC_SYSROOT)/lib/crtn.o: user/tcc_crtn.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -c $< -o $@

# libtcc1.a: TCC's runtime helper library (64-bit divide/shift, float<->int
# conversion helpers its i386 codegen calls out to). Built directly with
# the cross gcc rather than a bootstrapped tcc -- TCC's own lib/Makefile
# supports exactly this (`i386-libtcc1-usegcc=yes`), sidestepping the usual
# tcc-builds-its-own-libtcc1 chicken-and-egg entirely. bcheck.o (the
# bounds-checker's runtime half, only pulled in by `-b`) is deliberately
# omitted -- see third_party/tcc/README.md's patch list on CONFIG_TCC_BCHECK.
TCC_LIBTCC1_OBJS := $(addprefix $(BUILD)/user/tcc/,libtcc1.o alloca86.o alloca86-bt.o)

$(BUILD)/user/tcc/libtcc1.o: $(TCC_SRC)/lib/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/tcc/alloca86.o: $(TCC_SRC)/lib/alloca86.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/tcc/alloca86-bt.o: $(TCC_SRC)/lib/alloca86-bt.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -c $< -o $@

$(TCC_SYSROOT)/lib/libtcc1.a: $(TCC_LIBTCC1_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# Merged libc.a: newlib's vendored libc.a with newlib_syscalls.o's POSIX
# syscall names appended, so TCC's own unmodified default `-lc` link step
# resolves open/read/write/fork/... without any TCC-side patch to inject a
# second library -- exactly the pattern NEWLIB_ELFS/$(BUSYBOX_ELF) already
# use, just repackaged into the one archive TCC's default search expects.
$(TCC_SYSROOT)/lib/libc.a: $(NEWLIB_DIR)/lib/libc.a $(BUILD)/user/newlib_syscalls.o
	@mkdir -p $(dir $@)
	cp $(NEWLIB_DIR)/lib/libc.a $@
	$(AR) r $@ $(BUILD)/user/newlib_syscalls.o

$(TCC_SYSROOT)/lib/libm.a: $(NEWLIB_DIR)/lib/libm.a
	@mkdir -p $(dir $@)
	cp $< $@

# Headers: TCC's own 5 compiler-intrinsic headers (float.h/stddef.h/
# stdbool.h/stdarg.h/varargs.h) at TCC_SYSROOT_INCLUDE, PureUNIX's shadow
# headers (dirent.h, signal.h, sys/mman.h, ...) at TCC_COMPAT_INCLUDE, and
# newlib's full header tree at TCC_USR_INCLUDE -- the exact same 3-way
# precedence order (own headers, then compat shadows, then real newlib)
# $(NEWLIB_CFLAGS)'s -isystem ordering already uses at PureUNIX's own build
# time, just reproduced target-side so a running tcc finds the same headers.
$(TCC_SYSROOT)/include/.stamp: $(wildcard $(TCC_SRC)/include/*.h)
	@mkdir -p $(TCC_SYSROOT)/include
	cp $(TCC_SRC)/include/*.h $(TCC_SYSROOT)/include/
	@touch $@

$(TCC_SYSROOT)/compat-include/.stamp: $(shell find user/newlib_compat -type f)
	@mkdir -p $(TCC_SYSROOT)/compat-include
	cp -R user/newlib_compat/. $(TCC_SYSROOT)/compat-include/
	@touch $@

$(TCC_SYSROOT)/usr-include/.stamp: $(NEWLIB_DIR)/include
	@mkdir -p $(TCC_SYSROOT)/usr-include
	cp -R $(NEWLIB_DIR)/include/. $(TCC_SYSROOT)/usr-include/
	@touch $@

TCC_SYSROOT_FILES := $(TCC_SYSROOT)/lib/crt1.o $(TCC_SYSROOT)/lib/crti.o \
	$(TCC_SYSROOT)/lib/crtn.o $(TCC_SYSROOT)/lib/libtcc1.a \
	$(TCC_SYSROOT)/lib/libc.a $(TCC_SYSROOT)/lib/libm.a \
	$(TCC_SYSROOT)/include/.stamp $(TCC_SYSROOT)/compat-include/.stamp \
	$(TCC_SYSROOT)/usr-include/.stamp

tcc-sysroot: $(TCC_SYSROOT_FILES)

# Lua 5.4.7 (vendored source under third_party/lua/ -- see that dir's
# README.md for the vendoring rationale) -- real, unmodified upstream Lua,
# built the same way as TCC above: PureUNIX's own top-level Makefile
# compiles it directly rather than Lua's own src/Makefile (which assumes a
# hosted `cc`/`ar`/PLAT-guessing toolchain this cross build doesn't have),
# using the exact CORE_O/LIB_O object lists Lua's own Makefile defines.
# -DLUA_USE_POSIX takes Lua down its already-existing POSIX configuration
# branch (luaconf.h) -- real popen()/pclose(), fseeko/ftello, getc_unlocked/
# flockfile, sigaction-based Ctrl-C handling in the lua.c CLI -- all now
# backed by real PureUNIX syscalls/newlib glue (user/newlib_syscalls.c).
# LUA_USE_DLOPEN is deliberately left undefined: PureUNIX has no dynamic
# linker at all (same reasoning as TCC_PUREUNIX's CONFIG_TCC_STATIC
# default above), so loadlib.c compiles its own upstream "dynamic
# libraries not enabled" fallback (a real, complete branch of loadlib.c,
# not a patch) -- require() of pure-Lua modules from LUA_LDIR/LUA_CDIR
# (luaconf.h's default /usr/local/share|lib/lua/5.4/, installed below)
# still works fully; only requiring a compiled .so C module would fail,
# cleanly, with that upstream message. No Lua source files are patched.
LUA_SRC := third_party/lua/lua-5.4.7/src
LUA_CORE_SRCS := lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c \
	lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c \
	lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
LUA_LIB_SRCS := lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c \
	lmathlib.c loadlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c linit.c
LUA_BASE_OBJS := $(addprefix $(BUILD)/user/lua/,$(LUA_CORE_SRCS:.c=.o) $(LUA_LIB_SRCS:.c=.o))
LUA_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(LUA_SRC) -DLUA_USE_POSIX -DLUA_COMPAT_5_3

$(BUILD)/user/lua/%.o: $(LUA_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(LUA_BASE_OBJS:.o=.d) $(BUILD)/user/lua/lua.d $(BUILD)/user/lua/luac.d

LUA_ELF := $(BUILD)/user/lua.elf
LUAC_ELF := $(BUILD)/user/luac.elf

$(LUA_ELF): $(BUILD)/user/lua/lua.o $(LUA_BASE_OBJS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/lua/lua.o $(LUA_BASE_OBJS) \
		-Wl,--start-group -lc -lm -Wl,--end-group -lgcc -o $@

$(LUAC_ELF): $(BUILD)/user/lua/luac.o $(LUA_BASE_OBJS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/lua/luac.o $(LUA_BASE_OBJS) \
		-Wl,--start-group -lc -lm -Wl,--end-group -lgcc -o $@

# SQLite 3.53.3 (vendored amalgamation under third_party/sqlite/ -- see that
# dir's README.md for the vendoring rationale) -- real, unmodified upstream
# SQLite, built the same "vendor upstream source, compile with our own
# Makefile rules" pattern as TCC/Lua above. Unlike Lua's own many-small-
# files src/Makefile, SQLite's own upstream distribution *is* the
# amalgamation form (sqlite3.c + shell.c, exactly what sqlite.org packages
# for embedding) -- so there's no "own build system" to route around here;
# this just compiles the two files upstream already intends to be compiled
# together, the same way `gcc shell.c sqlite3.c -o sqlite3` in SQLite's own
# docs would, using i686-elf-gcc/newlib instead of a hosted toolchain.
#
# -DSQLITE_OS_UNIX=1 forces the unix VFS backend explicitly rather than
# relying on sqlite3.c's own `#if defined(_WIN32) ... #else unix#endif`
# autodetection (which would already land on unix here, since nothing
# defines _WIN32 on this freestanding target, but this is the same
# "declare platform intent explicitly" style as Lua's -DLUA_USE_POSIX).
# The real work backing that VFS -- POSIX advisory record locking via
# fcntl(F_SETLK/F_GETLK), which SQLite's default unix VFS needs for every
# transaction even with a single connection -- is include/pureunix/flock.h
# + kernel/flock.c (see docs/sqlite-port.md), not a stub.
#
# -DSQLITE_THREADSAFE=0: PureUNIX has no threading model (same reasoning as
# Lua's flockfile()/funlockfile() no-ops), so SQLite's internal mutexes
# compile out entirely rather than needing a pthread port.
# -DSQLITE_OMIT_LOAD_EXTENSION: no dynamic linker at all (same reasoning as
# Lua's LUA_USE_DLOPEN omission / TCC's CONFIG_TCC_STATIC) -- extension
# loading fails cleanly with upstream's own "not authorized" error; every
# other SQL feature is unaffected.
# -DSQLITE_OMIT_WAL: WAL mode needs a real shared-memory VFS layer
# (xShmMap/xShmLock, normally an mmap'd -shm file coordinating multiple
# processes) this port doesn't implement; the default rollback-journal
# mode (what every `sqlite3 file.db` session uses unless it explicitly
# opts into `PRAGMA journal_mode=WAL`) needs none of that and works fully.
# -DSQLITE_MAX_MMAP_SIZE=0: PureUNIX's mmap() (user/newlib_syscalls.c) only
# supports MAP_ANON|MAP_PRIVATE scratch allocations, not a real file-backed
# mapping SQLite's opportunistic mmap I/O path would need -- this disables
# that path cleanly rather than letting it fail at runtime.
# -DHAVE_USLEEP=1: real usleep() exists (user/newlib_syscalls.c, added for
# the Lua port) -- lets unixSleep() use real microsecond-resolution sleeps
# instead of rounding every busy-handler retry up to a whole second.
SQLITE_SRC := third_party/sqlite/sqlite-3.53.3
SQLITE_DEFS := -DSQLITE_OS_UNIX=1 -DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL \
	-DSQLITE_MAX_MMAP_SIZE=0 -DHAVE_USLEEP=1
SQLITE_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SQLITE_SRC) $(SQLITE_DEFS) -Wno-unused-function

$(BUILD)/user/sqlite/%.o: $(SQLITE_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_CFLAGS) -MMD -MP -c $< -o $@

SQLITE_OBJS := $(BUILD)/user/sqlite/sqlite3.o $(BUILD)/user/sqlite/shell.o
DEPS += $(SQLITE_OBJS:.o=.d)

SQLITE_ELF := $(BUILD)/user/sqlite3.elf

$(SQLITE_ELF): $(SQLITE_OBJS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(SQLITE_OBJS) \
		-Wl,--start-group -lc -lm -Wl,--end-group -lgcc -o $@

# ncurses 6.5 (vendored *build output* under third_party/ncurses/i686-elf/ --
# see third_party/ncurses/README.md and tools/build-ncurses.sh for why this
# one dependency, unlike TCC/Lua/SQLite, is a prebuilt-and-vendored artifact
# instead of source compiled directly by this Makefile: the same "generated
# sources are real, upstream-produced build output, not something to
# hand-reproduce" reasoning third_party/newlib already uses). Headers +
# libncurses.a/libform.a/libmenu.a/libpanel.a are just another -isystem/-L
# pair, exactly like $(NEWLIB_DIR) itself, so any current or future
# NEWLIB_PROGRAMS-style userspace binary can link against them the same way.
NCURSES_DIR := third_party/ncurses/i686-elf
NCURSES_CFLAGS := $(NEWLIB_CFLAGS) -isystem $(NCURSES_DIR)/include
NCURSES_LDFLAGS := $(NEWLIB_LDFLAGS) -L$(NCURSES_DIR)/lib

$(BUILD)/user/ncdemo.o: user/ncdemo.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NCURSES_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/ncdemo.d

NCDEMO_ELF := $(BUILD)/user/ncdemo.elf

$(NCDEMO_ELF): $(BUILD)/user/ncdemo.o $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NCURSES_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/ncdemo.o \
		-Wl,--start-group -lncurses -lc -lm -Wl,--end-group -lgcc -o $@

# htop 3.5.1 (vendored upstream source under third_party/htop/ -- see that
# directory's README.md and docs/htop-port.md). Unlike ncurses, htop's own
# build generates no real source at build time -- picking a platform just
# selects which small subdirectory's .c files join one fixed, hand-
# enumerable core file list (Makefile.am's `myhtopsources`) -- so this
# follows the TCC/Lua/SQLite "vendor upstream source, compile it with our
# own Makefile rules, no upstream build system" pattern instead, with a
# hand-written config.h (third_party/htop/pureunix/config.h) standing in
# for the one htop's own `configure` would otherwise generate -- see that
# file's own header comment for why. third_party/htop/pureunix/ is a real,
# new htop platform backend (Platform.c/Machine.c/Process.c/ProcessTable.c,
# the same plugin shape linux/, freebsd/, etc. already use) reading
# PureUNIX's own real /proc (fs/procfs.c), not a patch to any upstream
# htop file and not a fake Linux /proc.
HTOP_SRC := third_party/htop/htop-3.5.1
HTOP_PUREUNIX := third_party/htop/pureunix
HTOP_CORE_SRCS := Action.c Affinity.c AffinityPanel.c AvailableColumnsPanel.c \
	AvailableMetersPanel.c BatteryMeter.c CategoriesPanel.c ColorsPanel.c \
	ColumnsPanel.c CommandLine.c CommandScreen.c CPUMeter.c CRT.c \
	DateTimeMeter.c DiskIOMeter.c DisplayOptionsPanel.c DynamicColumn.c \
	DynamicMeter.c DynamicScreen.c EnvScreen.c FileDescriptorMeter.c \
	FunctionBar.c Hashtable.c Header.c HeaderOptionsPanel.c HostnameMeter.c \
	History.c IncSet.c InfoScreen.c LineEditor.c ListItem.c \
	LoadAverageMeter.c Machine.c MainPanel.c MemoryMeter.c \
	MemorySwapMeter.c Meter.c MetersPanel.c NetworkIOMeter.c Object.c \
	OpenFilesScreen.c OptionItem.c Panel.c Process.c ProcessLocksScreen.c \
	ProcessTable.c Row.c RichString.c Scheduling.c ScreenManager.c \
	ScreensPanel.c ScreenTabsPanel.c Settings.c SignalsPanel.c SwapMeter.c \
	SysArchMeter.c Table.c TasksMeter.c TraceScreen.c UptimeMeter.c \
	UsersTable.c Vector.c XUtils.c
HTOP_PLATFORM_SRCS := Platform.c PureUnixMachine.c PureUnixProcess.c PureUnixProcessTable.c
HTOP_OBJS := $(addprefix $(BUILD)/user/htop/,$(HTOP_CORE_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/htop/,$(HTOP_PLATFORM_SRCS:.c=.o)) \
	$(BUILD)/user/htop/htop.o \
	$(BUILD)/user/htop/generic/gettime.o

$(BUILD)/user/htop/generic/gettime.o: $(HTOP_SRC)/generic/gettime.c
	@mkdir -p $(dir $@)
	$(CC) $(HTOP_CFLAGS) -MMD -MP -c $< -o $@
HTOP_CFLAGS := $(USER_CFLAGS) $(NCURSES_CFLAGS) -D_GNU_SOURCE -DHAVE_CONFIG_H \
	-Ithird_party/htop -I$(HTOP_PUREUNIX) -I$(HTOP_SRC) \
	-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function

$(BUILD)/user/htop/%.o: $(HTOP_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HTOP_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/htop/%.o: $(HTOP_PUREUNIX)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HTOP_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(HTOP_OBJS:.o=.d)

HTOP_ELF := $(BUILD)/user/htop.elf

$(HTOP_ELF): $(HTOP_OBJS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NCURSES_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(HTOP_OBJS) \
		-Wl,--start-group -lncurses -lc -lm -Wl,--end-group -lgcc -o $@

# SDL2 2.28.5 (vendored upstream source under third_party/SDL2/ -- see that
# directory's README.md and docs/sdl-port.md). Same "vendor upstream
# source, compile it with our own Makefile rules, no upstream build
# system" pattern as TCC/Lua/SQLite/htop above -- SDL2's own configure/
# CMake both assume a hosted target and have no PureUNIX case (see the
# README's "Why not SDL's own build system"); this instead hand-lists the
# exact upstream .c files this platform's feature set needs (core, common
# video/render/events/timer machinery, the software renderer, every
# "dummy" stub backend for a not-yet-supported subsystem) plus one new
# real backend this port adds, third_party/SDL2/SDL2-2.28.5/src/video/
# pureunix/ (framebuffer/input glue, docs/sdl-port.md) and src/timer/
# pureunix/ (real SYS_GET_TICKS_MS-backed timing). include/SDL_config_
# pureunix.h (also new, not a patch) is this platform's hand-written
# equivalent of what configure/CMake would otherwise generate.
SDL_SRC := third_party/SDL2/SDL2-2.28.5
SDL_CORE_SRCS := SDL.c SDL_assert.c SDL_dataqueue.c SDL_error.c SDL_guid.c \
	SDL_hints.c SDL_list.c SDL_log.c SDL_utils.c \
	atomic/SDL_atomic.c atomic/SDL_spinlock.c \
	audio/SDL_audio.c audio/SDL_audiocvt.c audio/SDL_audiodev.c \
	audio/SDL_audiotypecvt.c audio/SDL_mixer.c audio/SDL_wave.c \
	audio/dummy/SDL_dummyaudio.c \
	cpuinfo/SDL_cpuinfo.c \
	events/imKStoUCS.c events/SDL_clipboardevents.c events/SDL_displayevents.c \
	events/SDL_dropevents.c events/SDL_events.c events/SDL_gesture.c \
	events/SDL_keyboard.c events/SDL_keysym_to_scancode.c events/SDL_mouse.c \
	events/SDL_quit.c events/SDL_scancode_tables.c events/SDL_touch.c \
	events/SDL_windowevents.c \
	file/SDL_rwops.c \
	filesystem/dummy/SDL_sysfilesystem.c \
	haptic/SDL_haptic.c haptic/dummy/SDL_syshaptic.c \
	joystick/controller_type.c joystick/SDL_gamecontroller.c joystick/SDL_joystick.c \
	joystick/dummy/SDL_sysjoystick.c \
	loadso/dummy/SDL_sysloadso.c \
	locale/SDL_locale.c locale/dummy/SDL_syslocale.c \
	misc/SDL_url.c misc/dummy/SDL_sysurl.c \
	power/SDL_power.c \
	render/SDL_d3dmath.c render/SDL_render.c render/SDL_yuv_sw.c \
	render/software/SDL_blendfillrect.c render/software/SDL_blendline.c \
	render/software/SDL_blendpoint.c render/software/SDL_drawline.c \
	render/software/SDL_drawpoint.c render/software/SDL_render_sw.c \
	render/software/SDL_rotate.c render/software/SDL_triangle.c \
	sensor/SDL_sensor.c sensor/dummy/SDL_dummysensor.c \
	stdlib/SDL_crc16.c stdlib/SDL_crc32.c stdlib/SDL_getenv.c \
	stdlib/SDL_iconv.c stdlib/SDL_malloc.c stdlib/SDL_mslibc.c \
	stdlib/SDL_qsort.c stdlib/SDL_stdlib.c stdlib/SDL_string.c \
	stdlib/SDL_strtokr.c \
	thread/SDL_thread.c thread/generic/SDL_syscond.c thread/generic/SDL_sysmutex.c \
	thread/generic/SDL_syssem.c thread/generic/SDL_systhread.c thread/generic/SDL_systls.c \
	timer/SDL_timer.c timer/pureunix/SDL_systimer.c \
	video/SDL_blit_0.c video/SDL_blit_1.c video/SDL_blit_A.c video/SDL_blit_auto.c \
	video/SDL_blit_copy.c video/SDL_blit_N.c video/SDL_blit_slow.c video/SDL_blit.c \
	video/SDL_bmp.c video/SDL_clipboard.c video/SDL_egl.c video/SDL_fillrect.c \
	video/SDL_pixels.c video/SDL_rect.c video/SDL_RLEaccel.c video/SDL_shape.c \
	video/SDL_stretch.c video/SDL_surface.c video/SDL_video.c \
	video/SDL_vulkan_utils.c video/SDL_yuv.c video/yuv2rgb/yuv_rgb.c \
	video/pureunix/SDL_puvideo.c video/pureunix/SDL_puevents.c video/pureunix/SDL_pufb.c
SDL_OBJS := $(addprefix $(BUILD)/user/sdl2/,$(SDL_CORE_SRCS:.c=.o))
# -ffunction-sections/-fdata-sections + the sdltest link's -Wl,--gc-sections
# below: this port compiles in every dummy stub backend (audio, joystick,
# haptic, sensor, ...) alongside the real video/event/timer ones so
# SDL_Init()'s subsystem dispatch table always has *something* to call
# regardless of which SDL_INIT_* flags an app actually passes (see
# docs/sdl-port.md) -- correct, but it means a plain whole-.o static link
# pulls in every one of those subsystems' code whether or not a given app
# ever touches them. Per-function/data sections let the linker discard
# whatever a specific app's reachability graph from main() never reaches
# instead, which matters here: PureUNIX's EXT2 image
# (tools/mkext2.py's TOTAL_BLOCKS) is a fixed 8 MiB, and an SDL app
# statically linking the whole library unpruned would be considerably
# larger than every other installed program combined.
SDL_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -D__pureunix__ -Iuser \
	-I$(SDL_SRC)/include -I$(SDL_SRC)/src \
	-ffunction-sections -fdata-sections \
	-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-unused-variable

$(BUILD)/user/sdl2/%.o: $(SDL_SRC)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(SDL_OBJS:.o=.d)

SDL_LIB := $(BUILD)/user/sdl2/libSDL2.a

$(SDL_LIB): $(SDL_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(SDL_OBJS)

$(BUILD)/user/sdltest.o: user/sdltest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -ffunction-sections -fdata-sections -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/sdltest.d

SDLTEST_ELF := $(BUILD)/user/sdltest.elf

$(SDLTEST_ELF): $(BUILD)/user/sdltest.o $(SDL_LIB) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) -Wl,--gc-sections $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/sdltest.o \
		-Wl,--start-group $(SDL_LIB) -lc -lm -Wl,--end-group -lgcc -o $@

# pude: PureUNIX's graphical desktop shell + PUTerm terminal emulator
# (user/pude.c + user/pude_term.c/.h, docs/pude.md) -- launches into the
# real SDL2 port above and runs a real BusyBox ash under a real PTY
# (include/pureunix/pty.h, kernel/pty.c). Reuses drivers/font.c's exact
# pre-rasterized glyph bitmap by compiling that same source file as its own
# object here (not a copy) -- see user/pude.c's own comment for why it
# can't be #include'd the normal way from a newlib translation unit
# (include/pureunix/font.h transitively pulls in include/pureunix/types.h,
# whose kernel-style uid_t/gid_t/... collide with newlib's <sys/types.h>).
$(BUILD)/user/pude_font.o: drivers/font.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_font.d

$(BUILD)/user/pude_term.o: user/pude_term.c user/pude_term.h user/pude_app.h user/pude_gfx.h user/pude_icon.h user/pureunix_pty.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_term.d

# pude_calc: the ring-3 GUI calculator app (user/pude_calc.c/.h, docs/pude.md)
# -- pure userspace arithmetic + rendering, plugged into the WM through the
# same app_class_t (user/pude_app.h) PUTerm uses.
$(BUILD)/user/pude_calc.o: user/pude_calc.c user/pude_calc.h user/pude_app.h user/pude_gfx.h user/pude_icon.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_calc.d

# pude_launch: the smallest general mechanism for a pude app to hand the
# whole screen to a real external program (e.g. PUFiles opening a .png
# with imgview) and cleanly resume afterward -- see pude_launch.c's own
# comment for why blocking the WM's entire event loop is correct here, not
# a shortcut. Plain fork()/execve()/waitpid(), no SDL dependency at all.
$(BUILD)/user/pude_launch.o: user/pude_launch.c user/pude_launch.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_launch.d

# pude_spawn: a tiny one-slot mailbox letting any pude app ask the WM to
# open a new window of a given app class (optionally preloaded with a
# startup command) -- see pude_spawn.h.
$(BUILD)/user/pude_spawn.o: user/pude_spawn.c user/pude_spawn.h user/pude_app.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_spawn.d

# pude_files: PUFiles, a real ring-3 graphical file manager (user/
# pude_files.c/.h, docs/pude.md) -- browses PureUNIX's real filesystem via
# ordinary opendir()/readdir()/stat()/mkdir()/rmdir()/unlink()/rename(),
# plugged into the WM through the same app_class_t PUTerm/Calculator use.
$(BUILD)/user/pude_files.o: user/pude_files.c user/pude_files.h user/pude_app.h user/pude_gfx.h user/pude_icon.h user/pude_widgets.h user/pude_launch.h user/pude_spawn.h user/pude_text.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_files.d

# pude_clipboard: a tiny desktop-session-local, text-only clipboard shared
# by every pude app (user/pude_clipboard.c/.h, docs/pude.md's "PUText"
# section) -- real shared mutable state, same convention as pude_spawn.c.
$(BUILD)/user/pude_clipboard.o: user/pude_clipboard.c user/pude_clipboard.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_clipboard.d

# pude_text: PUText, a real ring-3 graphical text editor (user/
# pude_text.c/.h, docs/pude.md) -- a dynamic line-array document buffer,
# real Open/Save/Save-As via the embedded pude_filepicker.h widget, and the
# shared pude_clipboard.h for Copy/Cut/Paste, plugged into the WM through
# the same app_class_t PUTerm/Calculator/PUFiles use.
$(BUILD)/user/pude_text.o: user/pude_text.c user/pude_text.h user/pude_app.h user/pude_gfx.h user/pude_icon.h user/pude_widgets.h user/pude_filepicker.h user/pude_clipboard.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_text.d

# pude_settings: Settings, the ring-3 GUI app exposing the desktop
# wallpaper setting (user/pude_settings.c/.h, docs/pude.md) -- plugged
# into the WM through the same app_class_t PUTerm/Calculator/PUFiles/
# PUText use. Drives user/pude_wallpaper.h's public API; never touches
# libpng/zlib itself (that dependency is confined to pude_wallpaper.o).
$(BUILD)/user/pude_settings.o: user/pude_settings.c user/pude_settings.h user/pude_app.h user/pude_gfx.h user/pude_icon.h user/pude_widgets.h user/pude_filepicker.h user/pude_wallpaper.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_settings.d

$(BUILD)/user/pude.o: user/pude.c user/pude_app.h user/pude_gfx.h user/pude_icon.h user/pude_term.h user/pude_calc.h user/pude_files.h user/pude_settings.h user/pude_spawn.h user/pude_text.h user/pude_wallpaper.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -Iuser -ffunction-sections -fdata-sections -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude.d

PUDE_ELF := $(BUILD)/user/pude.elf

$(PUDE_ELF): $(BUILD)/user/pude.o $(BUILD)/user/pude_term.o $(BUILD)/user/pude_calc.o $(BUILD)/user/pude_files.o $(BUILD)/user/pude_text.o $(BUILD)/user/pude_settings.o $(BUILD)/user/pude_wallpaper.o $(BUILD)/user/pude_launch.o $(BUILD)/user/pude_spawn.o $(BUILD)/user/pude_clipboard.o $(BUILD)/user/pude_font.o $(SDL_LIB) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) -Wl,--gc-sections $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/pude.o $(BUILD)/user/pude_term.o $(BUILD)/user/pude_calc.o $(BUILD)/user/pude_files.o $(BUILD)/user/pude_text.o $(BUILD)/user/pude_settings.o $(BUILD)/user/pude_wallpaper.o $(BUILD)/user/pude_launch.o $(BUILD)/user/pude_spawn.o $(BUILD)/user/pude_clipboard.o $(BUILD)/user/pude_font.o \
		-Wl,--start-group $(SDL_LIB) $(LIBPNG_LIB) $(ZLIB_LIB) -lc -lm -Wl,--end-group -lgcc -o $@

# $(LIBPNG_LIB)/$(ZLIB_LIB) aren't defined until further down this file
# (see the zlib/libpng port section) -- this second, recipe-less rule for
# the same target only adds them as *prerequisites* (so `make` rebuilds
# pude.elf if either static lib changes); the actual link recipe above
# already references both by name, which is safe regardless of where in
# this file they're defined (recipes expand at build time, once the whole
# Makefile has been read, unlike a prerequisite list).
$(PUDE_ELF): $(LIBPNG_LIB) $(ZLIB_LIB)

# Chocolate Doom 3.1.1 (vendored upstream source under
# third_party/chocolate-doom/ -- see that directory's README.md and
# docs/chocolate-doom-port.md). Same "vendor upstream source, compile it
# with our own Makefile rules, no upstream build system" pattern as every
# other third_party port -- the file lists below are copied directly from
# upstream's own src/CMakeLists.txt / src/doom/CMakeLists.txt so this stays
# a faithful reproduction of the real link graph. Only the chocolate-doom
# binary is built (not chocolate-server/chocolate-setup/Heretic/Hexen/
# Strife) -- see docs/chocolate-doom-port.md for why the setup GUI isn't
# needed for real gameplay.
CHOCDOOM_SRC := third_party/chocolate-doom/chocolate-doom-3.1.1
CHOCDOOM_CONFIG := third_party/chocolate-doom/config

CHOCDOOM_COMMON_SRCS := i_main.c i_system.c m_argv.c m_misc.c

CHOCDOOM_GAME_SRCS := aes_prng.c d_event.c d_iwad.c d_loop.c d_mode.c \
	deh_str.c gusconf.c i_cdmus.c i_endoom.c i_flmusic.c i_glob.c \
	i_input.c i_joystick.c i_musicpack.c i_oplmusic.c i_pcsound.c \
	i_sdlmusic.c i_sdlsound.c i_sound.c i_timer.c i_video.c i_videohr.c \
	i_winmusic.c midifallback.c midifile.c mus2mid.c m_bbox.c m_cheat.c \
	m_config.c m_controls.c m_fixed.c net_client.c net_common.c \
	net_dedicated.c net_gui.c net_io.c net_loop.c net_packet.c \
	net_petname.c net_query.c net_sdl.c net_server.c net_structrw.c \
	sha1.c memio.c tables.c v_diskicon.c v_video.c w_checksum.c w_main.c \
	w_wad.c w_file.c w_file_stdc.c w_file_posix.c w_merge.c z_zone.c

CHOCDOOM_DEH_SRCS := deh_io.c deh_main.c deh_mapping.c deh_text.c

CHOCDOOM_DOOM_SRCS := am_map.c deh_ammo.c deh_bexstr.c deh_cheat.c \
	deh_doom.c deh_frame.c deh_misc.c deh_ptr.c deh_sound.c deh_thing.c \
	deh_weapon.c d_items.c d_main.c d_net.c doomdef.c doomstat.c \
	dstrings.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c \
	m_menu.c m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c \
	p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c \
	p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c \
	p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c \
	r_segs.c r_sky.c r_things.c s_sound.c sounds.c statdump.c st_lib.c \
	st_stuff.c wi_stuff.c

CHOCDOOM_OPL_SRCS := opl.c opl_linux.c opl_obsd.c opl_queue.c opl_sdl.c \
	opl_timer.c opl_win32.c ioperm_sys.c opl3.c

CHOCDOOM_PCSOUND_SRCS := pcsound.c pcsound_bsd.c pcsound_sdl.c \
	pcsound_linux.c pcsound_win32.c

CHOCDOOM_TEXTSCREEN_SRCS := txt_conditional.c txt_checkbox.c txt_desktop.c \
	txt_dropdown.c txt_fileselect.c txt_gui.c txt_inputbox.c txt_io.c \
	txt_button.c txt_label.c txt_radiobutton.c txt_scrollpane.c \
	txt_separator.c txt_spinctrl.c txt_sdl.c txt_strut.c txt_table.c \
	txt_utf8.c txt_widget.c txt_window.c txt_window_action.c

CHOCDOOM_OBJS := \
	$(addprefix $(BUILD)/user/chocdoom/,$(CHOCDOOM_COMMON_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/,$(CHOCDOOM_GAME_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/,$(CHOCDOOM_DEH_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/doom/,$(CHOCDOOM_DOOM_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/opl/,$(CHOCDOOM_OPL_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/pcsound/,$(CHOCDOOM_PCSOUND_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/user/chocdoom/textscreen/,$(CHOCDOOM_TEXTSCREEN_SRCS:.c=.o))

CHOCDOOM_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -D__pureunix__ -Iuser \
	-I$(SDL_SRC)/include -I$(CHOCDOOM_CONFIG) -I$(CHOCDOOM_SRC)/src \
	-I$(CHOCDOOM_SRC)/src/doom -I$(CHOCDOOM_SRC)/opl \
	-I$(CHOCDOOM_SRC)/pcsound -I$(CHOCDOOM_SRC)/textscreen \
	-ffunction-sections -fdata-sections \
	-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
	-Wno-unused-variable -Wno-unused-but-set-variable

$(BUILD)/user/chocdoom/%.o: $(CHOCDOOM_SRC)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHOCDOOM_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/chocdoom/opl/%.o: $(CHOCDOOM_SRC)/opl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHOCDOOM_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/chocdoom/pcsound/%.o: $(CHOCDOOM_SRC)/pcsound/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHOCDOOM_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/chocdoom/textscreen/%.o: $(CHOCDOOM_SRC)/textscreen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHOCDOOM_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(CHOCDOOM_OBJS:.o=.d)

CHOCDOOM_ELF := $(BUILD)/user/chocolate-doom.elf

# Real, freely-distributable id Software shareware IWAD (Doom v1.9,
# sha1 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d — see docs/
# chocolate-doom-port.md's Testing section) — shipped at /bin/doom1.wad
# so `chocolate-doom -iwad doom1.wad` works immediately from any PATH
# directory with zero setup on a freshly booted image.
CHOCDOOM_IWAD := third_party/chocolate-doom/doom1.wad

$(CHOCDOOM_ELF): $(CHOCDOOM_OBJS) $(SDL_LIB) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) -Wl,--gc-sections $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(CHOCDOOM_OBJS) \
		-Wl,--start-group $(SDL_LIB) -lc -lm -Wl,--end-group -lgcc -o $@

# zlib 1.3.2 (vendored upstream source under third_party/zlib/ -- see that
# directory's README.md). Same "vendor upstream source, compile it with our
# own Makefile rules, no upstream build system" pattern as every other
# third_party port here -- zlib's own ./configure only ever regenerates a
# Makefile and (redundantly) zconf.h from zconf.h.in, and the release
# tarball already ships both pre-generated identically (diff zconf.h
# zconf.h.in is empty), so unlike TCC/Lua/SQLite there isn't even a
# configure-generated artifact being stood in for here.
#
# -DHAVE_UNISTD_H=1: zconf.h only defines Z_HAVE_UNISTD_H (which gzlib.c/
# gzread.c/gzwrite.c need for their real read()/write()/close()/lseek()
# calls) from a real ./configure's HAVE_UNISTD_H substitution, or a couple
# of hardcoded compiler checks (Watcom, DJGPP) that don't apply to this
# target -- the same "declare a real, true platform capability via -D
# instead of patching source" reasoning as SQLite's -DHAVE_USLEEP=1 above
# (newlib provides a real, complete POSIX unistd.h here).
ZLIB_SRC := third_party/zlib/zlib-1.3.2
ZLIB_SRCS := adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c \
	gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c \
	uncompr.c zutil.c
ZLIB_OBJS := $(addprefix $(BUILD)/user/zlib/,$(ZLIB_SRCS:.c=.o))
ZLIB_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -DHAVE_UNISTD_H=1 -I$(ZLIB_SRC)

$(BUILD)/user/zlib/%.o: $(ZLIB_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ZLIB_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(ZLIB_OBJS:.o=.d)

ZLIB_LIB := $(BUILD)/user/zlib/libz.a

$(ZLIB_LIB): $(ZLIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(ZLIB_OBJS)

# libpng 1.6.58 (vendored upstream source under third_party/libpng/ -- see
# that directory's README.md), built directly against $(ZLIB_SRC)/$(ZLIB_LIB)
# above -- same pattern again. libpng's own ./configure mainly exists to
# generate pnglibconf.h from scripts/pnglibconf.dfa via awk; upstream ships
# a ready-made stand-in for exactly this "no configure" situation --
# scripts/pnglibconf.h.prebuilt, "the configuration used to build the
# distributed dll and lib files" per libpng's own INSTALL doc (the same
# "official escape hatch for skipping configure", not a project invention,
# that ncurses/tcc/htop each use their own version of) -- copied into the
# build tree below as a real generated build artifact, never into the
# vendored source tree itself.
LIBPNG_SRC := third_party/libpng/libpng-1.6.58
LIBPNG_BUILD := $(BUILD)/user/libpng
LIBPNG_SRCS := png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c \
	pngrio.c pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c \
	pngwtran.c pngwutil.c
LIBPNG_OBJS := $(addprefix $(LIBPNG_BUILD)/,$(LIBPNG_SRCS:.c=.o))
LIBPNG_CFLAGS := $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(LIBPNG_BUILD) -I$(LIBPNG_SRC) -I$(ZLIB_SRC)

$(LIBPNG_BUILD)/pnglibconf.h: $(LIBPNG_SRC)/scripts/pnglibconf.h.prebuilt
	@mkdir -p $(dir $@)
	cp $< $@

$(LIBPNG_BUILD)/%.o: $(LIBPNG_SRC)/%.c $(LIBPNG_BUILD)/pnglibconf.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBPNG_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(LIBPNG_OBJS:.o=.d)

LIBPNG_LIB := $(LIBPNG_BUILD)/libpng16.a

$(LIBPNG_LIB): $(LIBPNG_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(LIBPNG_OBJS)

# pude_wallpaper: `pude`'s desktop wallpaper (user/pude_wallpaper.c/.h,
# docs/pude.md's "Settings" section) -- decodes a user-chosen PNG with the
# same real upstream libpng/zlib ports above (not a copy: same $(LIBPNG_LIB)/
# $(ZLIB_LIB) archives imgview links against), so it has to be compiled
# after both are defined here rather than alongside the rest of `pude`'s
# other app_class_t object rules further up this file.
$(BUILD)/user/pude_wallpaper.o: user/pude_wallpaper.c user/pude_wallpaper.h $(LIBPNG_BUILD)/pnglibconf.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(SDL_SRC)/include -I$(LIBPNG_BUILD) -I$(LIBPNG_SRC) -I$(ZLIB_SRC) -Iuser -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/pude_wallpaper.d

# Real zlib/libpng headers + static libs installed onto the EXT2 image at
# the same /usr/include, /usr/lib TCC_SYSROOT_FILES already populates with
# newlib's own headers/libc.a (TCC_USR_INCLUDE/TCC_USR_LIB above) -- so both
# a future third_party/ Makefile-based port (via $(ZLIB_SRC)/$(ZLIB_LIB),
# $(LIBPNG_SRC)/$(LIBPNG_LIB) directly, the same NCURSES_DIR-style pair this
# port itself uses) and a bare on-target `tcc foo.c -lpng16 -lz` can compile
# and link against real zlib/libpng, exactly as docs/imgview.md documents.
ZLIB_LIBPNG_EXTRA_FILES := \
	--extra-file $(ZLIB_SRC)/zlib.h:/usr/include/zlib.h \
	--extra-file $(ZLIB_SRC)/zconf.h:/usr/include/zconf.h \
	--extra-file $(ZLIB_LIB):/usr/lib/libz.a \
	--extra-file $(LIBPNG_SRC)/png.h:/usr/include/png.h \
	--extra-file $(LIBPNG_SRC)/pngconf.h:/usr/include/pngconf.h \
	--extra-file $(LIBPNG_BUILD)/pnglibconf.h:/usr/include/pnglibconf.h \
	--extra-file $(LIBPNG_LIB):/usr/lib/libpng16.a
ZLIB_LIBPNG_EXTRA_DEPS := $(ZLIB_SRC)/zlib.h $(ZLIB_SRC)/zconf.h $(ZLIB_LIB) \
	$(LIBPNG_SRC)/png.h $(LIBPNG_SRC)/pngconf.h $(LIBPNG_BUILD)/pnglibconf.h $(LIBPNG_LIB)

# imgview: native PNG viewer (user/imgview.c, docs/imgview.md) -- the whole
# point of porting zlib/libpng above. Links against both static libs plus
# newlib, exactly like $(NCDEMO_ELF) links against ncurses.
IMGVIEW_CFLAGS := $(NEWLIB_CFLAGS) -I$(LIBPNG_BUILD) -I$(LIBPNG_SRC) -I$(ZLIB_SRC) -Iuser

$(BUILD)/user/imgview.o: user/imgview.c user/pureunix_gfx.h $(LIBPNG_BUILD)/pnglibconf.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(IMGVIEW_CFLAGS) -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/imgview.d

IMGVIEW_ELF := $(BUILD)/user/imgview.elf

$(IMGVIEW_ELF): $(BUILD)/user/imgview.o $(LIBPNG_LIB) $(ZLIB_LIB) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/imgview.o \
		-Wl,--start-group $(LIBPNG_LIB) $(ZLIB_LIB) -lc -lm -Wl,--end-group -lgcc -o $@

# ziptest: standalone zlib compress()/uncompress() roundtrip regression
# check (user/ziptest.c, docs/zlib-port.md) -- a regression-test binary like
# systest.elf/opentest.elf, not an end-user command, so no /bin symlink.
$(BUILD)/user/ziptest.o: user/ziptest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -I$(ZLIB_SRC) -MMD -MP -c $< -o $@

DEPS += $(BUILD)/user/ziptest.d

ZIPTEST_ELF := $(BUILD)/user/ziptest.elf

$(ZIPTEST_ELF): $(BUILD)/user/ziptest.o $(ZLIB_LIB) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o \
		$(BUILD)/user/ziptest.o \
		-Wl,--start-group $(ZLIB_LIB) -lc -lm -Wl,--end-group -lgcc -o $@

.PHONY: all run run-live run-test iso live-iso test-persistent clean disk docs tcc-sysroot

all: $(KERNEL) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) $(TCC_ELF) $(LUA_ELF) $(LUAC_ELF) $(SQLITE_ELF) $(NCDEMO_ELF) $(HTOP_ELF) $(SDLTEST_ELF) $(PUDE_ELF) $(CHOCDOOM_ELF) $(IMGVIEW_ELF) $(ZIPTEST_ELF) $(DISK) $(DISK2)

$(KERNEL): $(KERNEL_OBJS) boot/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -lgcc -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/crt0.o: user/crt0.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/libpure.o: user/libpure.c user/libpure.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(BUILD)/user/crt0.o $(BUILD)/user/libpure.o user/linker.ld
	$(LD) $(USER_LDFLAGS) $(BUILD)/user/crt0.o $(BUILD)/user/libpure.o $< -lgcc -o $@

# Newlib-linked programs (see NEWLIB_PROGRAMS above): newlib_crt0.S's _start
# reads the raw argc/argv/envp frame kernel/elf.c's build_argv_stack() left
# on the initial stack (same trick user/crt0.S uses) and hands them to
# newlib_crt0.c's _start_c, which sets environ and calls exit(main(argc,
# argv)) instead of trapping straight to int $0x81. newlib_syscalls.o
# supplies the POSIX syscall names newlib itself doesn't provide for this
# target — see user/newlib_syscalls.c's header comment. Static pattern rules
# (restricted to NEWLIB_PROG_OBJS/NEWLIB_ELFS) rather than plain %-rules, so
# they don't clash with the libpure %.o/%.elf rules above for every other
# user program.
$(BUILD)/user/newlib_crt0.o: user/newlib_crt0.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/newlib_crt0_asm.o: user/newlib_crt0.S
	@mkdir -p $(dir $@)
	$(AS) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/newlib_syscalls.o: user/newlib_syscalls.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -MMD -MP -c $< -o $@

NEWLIB_PROG_OBJS := $(addprefix $(BUILD)/user/,$(addsuffix .o,$(NEWLIB_PROGRAMS)))

$(NEWLIB_PROG_OBJS): $(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(NEWLIB_CFLAGS) -MMD -MP -c $< -o $@

$(NEWLIB_ELFS): $(BUILD)/user/%.elf: $(BUILD)/user/%.o $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(NEWLIB_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o $< \
		-Wl,--start-group -lc -lm -Wl,--end-group -lgcc -o $@

# C++ programs (see NEWLIB_CXX_PROGRAMS below): same newlib_crt0/
# newlib_syscalls startup as the C newlib programs above, but compiled with
# $(CXX) against the vendored libstdc++ (LIBSTDCXX_CFLAGS/LDFLAGS) and
# linked with $(CXX) as the driver rather than $(LD)=$(CC), so it pulls in
# libstdc++/libsupc++'s own exception-handling glue correctly.
NEWLIB_CXX_PROGRAMS := cxxtest
NEWLIB_CXX_ELFS := $(addprefix $(BUILD)/user/,$(addsuffix .elf,$(NEWLIB_CXX_PROGRAMS)))
NEWLIB_CXX_PROG_OBJS := $(addprefix $(BUILD)/user/,$(addsuffix .o,$(NEWLIB_CXX_PROGRAMS)))

$(NEWLIB_CXX_PROG_OBJS): $(BUILD)/user/%.o: user/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(USER_CXXFLAGS) $(LIBSTDCXX_CFLAGS) -MMD -MP -c $< -o $@

$(NEWLIB_CXX_ELFS): $(BUILD)/user/%.elf: $(BUILD)/user/%.o $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o user/linker.ld
	@mkdir -p $(dir $@)
	$(CXX) $(LIBSTDCXX_LDFLAGS) $(BUILD)/user/newlib_crt0_asm.o $(BUILD)/user/newlib_crt0.o $(BUILD)/user/newlib_syscalls.o $< \
		-Wl,--start-group -lstdc++ -lsupc++ -lc -lm -Wl,--end-group -lgcc -o $@

DEPS += $(NEWLIB_CXX_PROG_OBJS:.o=.d)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

DOCS_DIR := docs
DOCS_MD := $(shell find $(DOCS_DIR) -name '*.md' 2>/dev/null)

# extra-files/: a local drop-in directory for staging arbitrary host files
# (e.g. a custom desktop wallpaper PNG for Settings, docs/pude.md's
# "Settings" section) onto the OS image with no Makefile edit needed --
# every file found under here is copied onto the image at the same path
# relative to /, e.g. extra-files/wallpaper.png lands at /wallpaper.png,
# extra-files/home/photo.png lands at /home/photo.png. Uses tools/
# mkext2.py's existing generic --extra-file HOST:DEST mechanism (the same
# one $(CHOCDOOM_IWAD)/$(ZLIB_LIBPNG_EXTRA_FILES) already use above), so
# there's no second copy of that logic. Not version-controlled (see
# .gitignore) -- purely a local convenience directory, empty by default.
EXTRA_FILES_DIR := extra-files
EXTRA_FILES_HOST := $(shell find $(EXTRA_FILES_DIR) -type f -not -name '.*' 2>/dev/null | sort)
EXTRA_FILES_ARGS := $(foreach f,$(EXTRA_FILES_HOST),--extra-file $(f):/$(patsubst $(EXTRA_FILES_DIR)/%,%,$(f)))

$(DISK): $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) tools/mkfat16.py $(DOCS_MD)
	$(PYTHON) tools/mkfat16.py $@ --docs $(DOCS_DIR) $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS)

$(DISK2): $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) $(BUSYBOX_ELF) $(TCC_ELF) $(TCC_SYSROOT_FILES) $(LUA_ELF) $(LUAC_ELF) $(SQLITE_ELF) $(NCDEMO_ELF) $(HTOP_ELF) $(SDLTEST_ELF) $(PUDE_ELF) $(CHOCDOOM_ELF) $(CHOCDOOM_IWAD) $(IMGVIEW_ELF) $(ZIPTEST_ELF) $(ZLIB_LIBPNG_EXTRA_DEPS) $(EXTRA_FILES_HOST) tools/mkext2.py $(DOCS_MD)
	$(PYTHON) tools/mkext2.py $@ --docs $(DOCS_DIR) $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) $(BUSYBOX_ELF) $(LUA_ELF) $(LUAC_ELF) $(SQLITE_ELF) $(NCDEMO_ELF) $(HTOP_ELF) $(SDLTEST_ELF) $(PUDE_ELF) $(CHOCDOOM_ELF) $(IMGVIEW_ELF) $(ZIPTEST_ELF) \
		--tcc-elf $(TCC_ELF) --tcc-sysroot $(TCC_SYSROOT) --extra-file $(CHOCDOOM_IWAD):/bin/doom1.wad $(ZLIB_LIBPNG_EXTRA_FILES) $(EXTRA_FILES_ARGS)

disk: $(DISK) $(DISK2)

# Persistent-disk EXT2 root: same builder, same content, plus /boot/
# pureunix.elf + a real /boot/grub/grub.cfg (see tools/mkext2.py's
# add_persistent_boot_files()) and the PUREUNIX_ROOT volume label
# boot/grub-embedded.cfg's `search` command looks for — this is what
# actually ends up as the writable root partition in $(ISO) below, as
# opposed to $(DISK2) which still only ever travels as an ephemeral GRUB
# ramdisk module in $(LIVE_ISO).
$(DISK_PERSISTENT): $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) $(BUSYBOX_ELF) $(TCC_ELF) $(TCC_SYSROOT_FILES) $(LUA_ELF) $(LUAC_ELF) $(SQLITE_ELF) $(NCDEMO_ELF) $(HTOP_ELF) $(SDLTEST_ELF) $(PUDE_ELF) $(CHOCDOOM_ELF) $(CHOCDOOM_IWAD) $(IMGVIEW_ELF) $(ZIPTEST_ELF) $(ZLIB_LIBPNG_EXTRA_DEPS) $(EXTRA_FILES_HOST) $(KERNEL) tools/mkext2.py $(DOCS_MD)
	$(PYTHON) tools/mkext2.py $@ --docs $(DOCS_DIR) $(USER_ELFS) $(NEWLIB_ELFS) $(NEWLIB_CXX_ELFS) $(BUSYBOX_ELF) $(LUA_ELF) $(LUAC_ELF) $(SQLITE_ELF) $(NCDEMO_ELF) $(HTOP_ELF) $(SDLTEST_ELF) $(PUDE_ELF) $(CHOCDOOM_ELF) $(IMGVIEW_ELF) $(ZIPTEST_ELF) \
		--tcc-elf $(TCC_ELF) --tcc-sysroot $(TCC_SYSROOT) --persistent-boot $(KERNEL) --extra-file $(CHOCDOOM_IWAD):/bin/doom1.wad $(ZLIB_LIBPNG_EXTRA_FILES) $(EXTRA_FILES_ARGS)

# Build-time GRUB core.img (i386-pc BIOS target): embeds boot/grub-
# embedded.cfg as its prefix config, which just `search`es for the
# PUREUNIX_ROOT-labeled partition and hands off to the real, on-disk
# /boot/grub/grub.cfg $(DISK_PERSISTENT) ships — so the actual boot menu
# lives as an ordinary file on the root filesystem, not baked into
# core.img. boot.img is GRUB's own prebuilt, unmodified 512-byte MBR
# bootstrap; both live under $(BUILD)/grub so tools/mkdiskimg.py has a
# stable place to find them. No -d flag: grub-mkimage already knows its
# own installed module directory (confirmed — see the module list below).
$(CORE_IMG): boot/grub-embedded.cfg
	@command -v $(GRUB_MKIMAGE) >/dev/null 2>&1 || { \
		echo "$(GRUB_MKIMAGE) is required for make iso. Install GRUB tools (e.g. 'brew install i686-elf-grub') and retry."; \
		exit 1; \
	}
	@mkdir -p $(BUILD)/grub
	$(GRUB_MKIMAGE) -O i386-pc -o $@ -c boot/grub-embedded.cfg -p /boot/grub \
		biosdisk part_msdos ext2 search fshelp normal configfile multiboot2

$(GRUB_BOOT_IMG):
	@mkdir -p $(BUILD)/grub
	@GRUB_BIN=$$($(PYTHON) -c "import os,shutil; print(os.path.realpath(shutil.which('$(GRUB_MKIMAGE)')))"); \
	cp "$$(dirname "$$(dirname "$$GRUB_BIN")")/lib/i686-elf/grub/i386-pc/boot.img" $@

# Boots through real GRUB (not QEMU's built-in -kernel multiboot1 loader,
# which never sets up a graphics mode) so the multiboot2 framebuffer request
# in boot/multiboot2.S actually takes effect. $(ISO) is a real, self-
# contained MBR disk image (tools/mkdiskimg.py — GRUB bootstrap + core.img
# in the MBR gap + a writable EXT2 root partition), so -drive+-boot c boots
# it exactly the way a real BIOS boots a flashed USB stick — no -cdrom, no
# separate module files, no ramdisk.
run: $(ISO)
	$(QEMU) -m 128M -drive file=$(ISO),format=raw -boot c \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-serial stdio -no-reboot -no-shutdown

# The original ISO9660+ramdisk-module boot, preserved unchanged for anyone
# who wants the old fast/ephemeral dev loop (root discarded every boot).
run-live: $(LIVE_ISO)
	$(QEMU) -m 128M -cdrom $(LIVE_ISO) -boot d \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-serial stdio -no-reboot -no-shutdown

# Headless, scripted boot for regression tests that need real keystrokes
# (Ctrl+C/Ctrl+Z, VT switching, login) — see tools/vt-inject-test.py's own
# header comment for the whole design (QEMU has no usable stdin for this;
# it drives a QMP socket instead) and tools/vt-scripts/*.txt for the
# actual test scripts. Each script boots its own fresh QEMU instance.
# Deliberately still $(LIVE_ISO), not $(ISO): every existing script assumes
# a fresh, ephemeral first-boot on every single run (see that file's own
# header comment) — unrelated to and unaffected by the new persistent-disk
# boot path, which has its own dedicated test (tools/test-persistent-boot.py).
run-test: $(LIVE_ISO)
	$(PYTHON) tools/vt-inject-test.py --iso $(LIVE_ISO) tools/vt-scripts/*.txt

# Self-contained: fat.img/root.img are embedded as GRUB modules (see
# boot/grub.cfg's `module2` lines) rather than passed to QEMU as separate
# -drive files, so the resulting ISO boots into the identical environment
# on its own. Kept exactly as before (see $(LIVE_ISO)'s own comment above)
# purely for run-test's regression suite.
$(LIVE_ISO): $(KERNEL) $(DISK) $(DISK2) boot/grub.cfg
	@command -v $(GRUB_MKRESCUE) >/dev/null 2>&1 || { \
		echo "$(GRUB_MKRESCUE) is required for make run-live/run-test. Install GRUB tools (e.g. 'brew install i686-elf-grub') and retry."; \
		exit 1; \
	}
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/pureunix.elf
	cp $(DISK) $(BUILD)/iso/boot/fat.img
	cp $(DISK2) $(BUILD)/iso/boot/root.img
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(LIVE_ISO) $(BUILD)/iso

live-iso: $(LIVE_ISO)

# The real, USB-flashable deliverable: dd $(ISO) onto a USB stick, boot it
# on real hardware, and it's already a persistent PureUnix install — no
# separate `install` step. See tools/mkdiskimg.py's own header comment for
# the exact on-disk layout and why it hand-implements GRUB's boot.img/
# core.img embedding protocol instead of shelling out to grub-bios-setup.
$(ISO): $(GRUB_BOOT_IMG) $(CORE_IMG) $(DISK_PERSISTENT) tools/mkdiskimg.py
	$(PYTHON) tools/mkdiskimg.py $@ --boot-img $(GRUB_BOOT_IMG) --core-img $(CORE_IMG) --ext2-img $(DISK_PERSISTENT)

iso: $(ISO)

# Boots $(ISO) twice (fresh QEMU process each time, against a scratch copy
# so the real deliverable stays pristine) proving root password + real
# filesystem writes survive an actual reboot — see tools/test-persistent-
# boot.py's own header comment for exactly what this does and why a false
# pass is essentially impossible.
test-persistent: $(ISO)
	cp $(ISO) $(BUILD)/persist-test.img
	$(PYTHON) tools/test-persistent-boot.py $(BUILD)/persist-test.img

clean:
	rm -rf $(BUILD)

-include $(DEPS)
