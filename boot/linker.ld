ENTRY(_start)

SECTIONS
{
    . = 1M;

    __kernel_start = .;

    .multiboot ALIGN(8) :
    {
        KEEP(*(.multiboot))
        KEEP(*(.multiboot2))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        *(.eh_frame*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
    }
}
