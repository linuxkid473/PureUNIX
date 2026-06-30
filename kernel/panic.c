#include <stdarg.h>
#include <pureunix/arch.h>
#include <pureunix/panic.h>
#include <pureunix/stdio.h>
#include <pureunix/vga.h>

void panic(const char *fmt, ...)
{
    arch_disable_interrupts();
    vga_set_color(VGA_WHITE, VGA_RED);
    printf("\n\nKERNEL PANIC: ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\nSystem halted.\n");
    for (;;) {
        arch_halt();
    }
}
