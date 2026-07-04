ENTRY(_start)

SECTIONS
{
    /* Must match USER_WINDOW_BASE in include/pureunix/vmm.h — every process
     * gets a private mapping of this same virtual range. */
    . = 0x08000000;
    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
}
