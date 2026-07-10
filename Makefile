CROSS ?= i686-elf-
CC := $(CROSS)gcc
AS := $(CROSS)gcc
LD := $(CROSS)gcc
AR := $(CROSS)ar
QEMU ?= qemu-system-i386
PYTHON ?= python3
GRUB_MKRESCUE ?= i686-elf-grub-mkrescue

BUILD := build
KERNEL := $(BUILD)/pureunix.elf
ISO := $(BUILD)/pureunix.iso
DISK  := $(BUILD)/pureunix.img
DISK2 := $(BUILD)/ext2.img

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

.PHONY: all run run-test iso clean disk docs tcc-sysroot

all: $(KERNEL) $(NEWLIB_ELFS) $(TCC_ELF) $(DISK) $(DISK2)

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

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

DOCS_DIR := docs
DOCS_MD := $(shell find $(DOCS_DIR) -name '*.md' 2>/dev/null)

$(DISK): $(USER_ELFS) $(NEWLIB_ELFS) tools/mkfat16.py $(DOCS_MD)
	$(PYTHON) tools/mkfat16.py $@ --docs $(DOCS_DIR) $(USER_ELFS) $(NEWLIB_ELFS)

$(DISK2): $(USER_ELFS) $(NEWLIB_ELFS) $(BUSYBOX_ELF) $(TCC_ELF) $(TCC_SYSROOT_FILES) tools/mkext2.py $(DOCS_MD)
	$(PYTHON) tools/mkext2.py $@ --docs $(DOCS_DIR) $(USER_ELFS) $(NEWLIB_ELFS) $(BUSYBOX_ELF) \
		--tcc-elf $(TCC_ELF) --tcc-sysroot $(TCC_SYSROOT)

disk: $(DISK) $(DISK2)

# Boots through real GRUB (not QEMU's built-in -kernel multiboot1 loader,
# which never sets up a graphics mode) so the multiboot2 framebuffer request
# in boot/multiboot2.S actually takes effect. $(ISO) is a fully standalone
# boot image (kernel + both disk images travel inside it as GRUB modules —
# see boot/grub.cfg and kernel_main's ramdisk_attach() calls), so -cdrom is
# the entire boot media; no separate -drive flags needed here, and this is
# the same ISO a real machine or VirtualBox/VMware would boot from a USB
# stick or CD.
run: $(ISO)
	$(QEMU) -m 128M -cdrom $(ISO) -boot d \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-serial stdio -no-reboot -no-shutdown

# Headless, scripted boot for regression tests that need real keystrokes
# (Ctrl+C/Ctrl+Z, VT switching, login) — see tools/vt-inject-test.py's own
# header comment for the whole design (QEMU has no usable stdin for this;
# it drives a QMP socket instead) and tools/vt-scripts/*.txt for the
# actual test scripts. Each script boots its own fresh QEMU instance.
run-test: $(ISO)
	$(PYTHON) tools/vt-inject-test.py --iso $(ISO) tools/vt-scripts/*.txt

# Self-contained: fat.img/root.img are embedded as GRUB modules (see
# boot/grub.cfg's `module2` lines) rather than passed to QEMU as separate
# -drive files, so the resulting ISO boots into the identical environment
# on its own — in QEMU, VirtualBox/VMware, or dd'd to a real USB stick.
$(ISO): $(KERNEL) $(DISK) $(DISK2) boot/grub.cfg
	@command -v $(GRUB_MKRESCUE) >/dev/null 2>&1 || { \
		echo "$(GRUB_MKRESCUE) is required for make iso/run. Install GRUB tools (e.g. 'brew install i686-elf-grub') and retry."; \
		exit 1; \
	}
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/pureunix.elf
	cp $(DISK) $(BUILD)/iso/boot/fat.img
	cp $(DISK2) $(BUILD)/iso/boot/root.img
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(BUILD)/iso

iso: $(ISO)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
