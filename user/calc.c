#include "libpure.h"

int main(void)
{
    int a = 144;
    int b = 12;
    pu_puts("calculator demo\n");
    pu_puts("144 + 12 = ");
    pu_puti(a + b);
    pu_puts("\n144 / 12 = ");
    pu_puti(a / b);
    pu_puts("\n");
    return 0;
}
