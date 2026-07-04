.set MBOOT1_MAGIC, 0x1BADB002
.set MBOOT1_FLAGS, 0x00000003
.set MBOOT1_CHECKSUM, -(MBOOT1_MAGIC + MBOOT1_FLAGS)

.section .multiboot, "a"
.align 4
.long MBOOT1_MAGIC
.long MBOOT1_FLAGS
.long MBOOT1_CHECKSUM

.set MBOOT2_MAGIC, 0xE85250D6
.set MBOOT2_ARCH, 0
.set MBOOT2_LENGTH, mboot2_end - mboot2_start
.set MBOOT2_CHECKSUM, -(MBOOT2_MAGIC + MBOOT2_ARCH + MBOOT2_LENGTH)

.section .multiboot2, "a"
.align 8
mboot2_start:
.long MBOOT2_MAGIC
.long MBOOT2_ARCH
.long MBOOT2_LENGTH
.long MBOOT2_CHECKSUM

# Request a linear graphics framebuffer instead of VGA text mode.
# Cell grid is 80x25 at 8x17 pixels/glyph (see include/pureunix/font.h).
.align 8
fb_tag_start:
.short 5        # MULTIBOOT_HEADER_TAG_FRAMEBUFFER
.short 0        # flags
.long fb_tag_end - fb_tag_start
.long 640       # width
.long 425       # height
.long 32        # depth
fb_tag_end:

.align 8
.short 0
.short 0
.long 8
mboot2_end:

.section .bss
.align 16
.global boot_stack_bottom
.global boot_stack_top
boot_stack_bottom:
.skip 32768
boot_stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $boot_stack_top, %esp
    xor %ebp, %ebp
    push %ebx
    push %eax
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

.size _start, . - _start
