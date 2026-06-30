#include <pureunix/arch.h>
#include <pureunix/config.h>
#include <pureunix/disk.h>
#include <pureunix/fat16.h>
#include <pureunix/kernel.h>
#include <pureunix/keyboard.h>
#include <pureunix/memory.h>
#include <pureunix/serial.h>
#include <pureunix/shell.h>
#include <pureunix/stdio.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    serial_init();
    vga_init();
    printf("\033[92m%s\033[0m %s for %s\n", PUREUNIX_NAME, PUREUNIX_VERSION, PUREUNIX_ARCH);
    printf("Boot magic=%x mbi=%p\n", magic, (void *)mbi_addr);

    arch_init();
    pmm_init(magic, mbi_addr);
    vmm_init();
    heap_init();
    tasking_init();
    syscall_init();
    pit_init(100);
    keyboard_init();
    ata_init();
    vfs_init();

    disk_device_t *disk = ata_primary_master();
    if (disk->present && fat16_mount(disk) == 0) {
        printf("Root filesystem mounted on %s\n", disk->name);
    } else {
        printf("No FAT16 disk mounted; filesystem commands will report errors.\n");
    }

    arch_enable_interrupts();
    shell_run();

    for (;;) {
        arch_halt();
    }
}
