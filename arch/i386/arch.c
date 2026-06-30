#include <pureunix/arch.h>

void arch_init(void)
{
    gdt_init();
    idt_init();
    pic_init();
}
