CROSS ?= i686-elf-
CC := $(CROSS)gcc
AS := $(CROSS)gcc
LD := $(CROSS)gcc
AR := $(CROSS)ar
QEMU ?= qemu-system-i386
PYTHON ?= python3

BUILD := build
KERNEL := $(BUILD)/pureunix.elf
ISO := $(BUILD)/pureunix.iso
DISK := $(BUILD)/pureunix.img

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

USER_PROGRAMS := hello calc viewer editor sh opentest
USER_ELFS := $(addprefix $(BUILD)/user/,$(addsuffix .elf,$(USER_PROGRAMS)))

.PHONY: all run iso clean disk docs

all: $(KERNEL) $(DISK)

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

disk: $(DISK)

run: $(KERNEL) $(DISK)
	$(QEMU) -m 128M -kernel $(KERNEL) -drive file=$(DISK),format=raw,if=ide,index=0 \
		-serial stdio -no-reboot -no-shutdown

iso: $(KERNEL) $(DISK)
	@command -v grub-mkrescue >/dev/null 2>&1 || { \
		echo "grub-mkrescue is required for make iso. Install GRUB tools and rerun make iso."; \
		exit 1; \
	}
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/pureunix.elf
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso

clean:
	rm -rf $(BUILD)

-include $(DEPS)
