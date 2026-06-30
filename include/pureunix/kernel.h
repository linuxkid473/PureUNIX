#ifndef PUREUNIX_KERNEL_H
#define PUREUNIX_KERNEL_H

#include <pureunix/types.h>

void kernel_main(uint32_t magic, uint32_t mbi_addr);
void kernel_reboot(void);
void kernel_shutdown(void);

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

#endif
