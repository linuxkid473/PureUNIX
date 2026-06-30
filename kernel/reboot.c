#include <pureunix/io.h>
#include <pureunix/kernel.h>
#include <pureunix/stdio.h>

void kernel_reboot(void)
{
    printf("rebooting...\n");
    while (inb(0x64) & 0x02) {
    }
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kernel_shutdown(void)
{
    printf("shutting down...\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;) {
        __asm__ volatile("hlt");
    }
}
