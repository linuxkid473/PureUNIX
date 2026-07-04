#include <pureunix/arch.h>
#include <pureunix/bootsplash.h>
#include <pureunix/config.h>
#include <pureunix/disk.h>
#include <pureunix/ext2.h>
#include <pureunix/fat16.h>
#include <pureunix/framebuffer.h>
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
    arch_init();
    pmm_init(magic, mbi_addr);

    /* Parse the framebuffer tag and map its physical range before any
     * console output, so vga_init()/bootsplash_show() below can safely
     * write pixels once paging is enabled. */
    fb_probe(magic, mbi_addr);
    const fb_info_t *fb = fb_get_info();
    vmm_init(fb->present ? fb->addr : 0, fb->present ? fb->pitch * fb->height : 0);

    vga_init();
    bootsplash_show();
    printf("\033[92m%s\033[0m %s for %s\n", PUREUNIX_NAME, PUREUNIX_VERSION, PUREUNIX_ARCH);
    printf("Boot magic=%x mbi=%p\n", magic, (void *)mbi_addr);

    heap_init();
    tasking_init();
    syscall_init();
    pit_init(100);
    keyboard_init();
    ata_init();
    vfs_init();

    /* FAT16 on primary master (ata0) — compatibility/testing only, mounted at /fat */
    disk_device_t *disk = ata_primary_master();
    if (disk->present && fat16_mount(disk) == 0) {
        vfs_mount("/fat", VFS_FS_FAT16, fat16_vfs_ops(), NULL);
        printf("FAT16 mounted on /fat\n");
    } else {
        printf("No FAT16 disk found; /fat will be unavailable.\n");
    }

    /* EXT2 on primary slave (ata1) — primary root filesystem */
    disk_device_t *disk2 = ata_primary_slave();
    if (disk2->present) {
        if (ext2_mount(disk2) == 0) {
            vfs_mount("/", VFS_FS_EXT2, ext2_vfs_ops(), NULL);
            printf("EXT2 mounted on /\n");
        } else {
            printf("EXT2 mount failed on %s; root filesystem unavailable.\n", disk2->name);
        }
    }

    arch_enable_interrupts();
    shell_run();

    for (;;) {
        arch_halt();
    }
}
