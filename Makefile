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

KERNEL_C_SRCS := $(shell find kernel arch drivers fs libc shell editor -name '*.c' | sort)
KERNEL_AS_SRCS := $(shell find boot arch -name '*.S' | sort)
KERNEL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C_SRCS)) \
	$(patsubst %.S,$(BUILD)/%.o,$(KERNEL_AS_SRCS))
DEPS := $(KERNEL_OBJS:.o=.d)

USER_PROGRAMS := hello calc viewer editor sh opentest readtest ext2test systest termiostest
USER_ELFS := $(addprefix $(BUILD)/user/,$(addsuffix .elf,$(USER_PROGRAMS)))

.PHONY: all run iso clean disk docs

all: $(KERNEL) $(DISK) $(DISK2)

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

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

DOCS_DIR := docs
DOCS_MD := $(shell find $(DOCS_DIR) -name '*.md' 2>/dev/null)

$(DISK): $(USER_ELFS) tools/mkfat16.py $(DOCS_MD)
	$(PYTHON) tools/mkfat16.py $@ --docs $(DOCS_DIR) $(USER_ELFS)

$(DISK2): $(USER_ELFS) tools/mkext2.py $(DOCS_MD)
	$(PYTHON) tools/mkext2.py $@ --docs $(DOCS_DIR) $(USER_ELFS)

disk: $(DISK) $(DISK2)

# Boots through real GRUB (not QEMU's built-in -kernel multiboot1 loader,
# which never sets up a graphics mode) so the multiboot2 framebuffer request
# in boot/multiboot2.S actually takes effect.
run: $(ISO) $(DISK) $(DISK2)
	$(QEMU) -m 128M -cdrom $(ISO) -boot d \
		-drive file=$(DISK),format=raw,if=ide,index=0 \
		-drive file=$(DISK2),format=raw,if=ide,index=1 \
		-serial stdio -no-reboot -no-shutdown

$(ISO): $(KERNEL) boot/grub.cfg
	@command -v $(GRUB_MKRESCUE) >/dev/null 2>&1 || { \
		echo "$(GRUB_MKRESCUE) is required for make iso/run. Install GRUB tools (e.g. 'brew install i686-elf-grub') and retry."; \
		exit 1; \
	}
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/pureunix.elf
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(BUILD)/iso

iso: $(ISO)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
