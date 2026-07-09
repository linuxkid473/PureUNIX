#include <pureunix/arch.h>
#include <pureunix/arp.h>
#include <pureunix/bootsplash.h>
#include <pureunix/config.h>
#include <pureunix/crypto.h>
#include <pureunix/disk.h>
#include <pureunix/e1000.h>
#include <pureunix/elf.h>
#include <pureunix/eth.h>
#include <pureunix/ext2.h>
#include <pureunix/fat16.h>
#include <pureunix/framebuffer.h>
#include <pureunix/icmp.h>
#include <pureunix/ip.h>
#include <pureunix/kernel.h>
#include <pureunix/keyboard.h>
#include <pureunix/memory.h>
#include <pureunix/panic.h>
#include <pureunix/pci.h>
#include <pureunix/ramdisk.h>
#include <pureunix/serial.h>
#include <pureunix/shell.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/tty.h>
#include <pureunix/users.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>
#include <pureunix/xhci.h>

/* The real login shell for an interactive session — BusyBox ash by default
 * (/etc/passwd's shell field, seeded "/bin/sh" — see kernel/users.c),
 * launched as a genuine ring-3 process via elf_exec_argv() exactly the way
 * the (now-secondary) in-kernel shell launches any other external program
 * (shell/sh.c's exec_external()); kernel_main() itself is "current" here
 * (tasking_init()'s main_task, not a user task) — see task_create_user()'s
 * fd/uid/gid/cwd inheritance, which works the same regardless. Blocks
 * (elf_exec_argv() only returns once its child has exited — see
 * kernel/elf.c) until the login shell itself exits, then kernel_main()'s
 * own for(;;) loop calls users_login() again, i.e. exiting the shell logs
 * you out back to a fresh login prompt, real-getty style.
 *
 * If the configured shell can't be exec'd at all (a stripped-down disk
 * image, or /etc/passwd pointing somewhere that no longer exists), falls
 * back to /bin/puresh (see docs/shell.md), then as an absolute last resort
 * to the legacy in-kernel shell_run() so the system is never unusable. */
/* Launches `path` as the login shell if it actually exists, treating any
 * exit code it returns (elf_exec_argv() propagates the child's own raw
 * exit code — including negative "killed by signal" codes, see SYS_KILL's
 * doc in docs/syscalls.md) as a normal, successful session rather than a
 * launch failure — the vfs_stat() gate is what actually distinguishes
 * "couldn't start this shell at all" from "started fine, ran, exited"
 * (elf_exec_argv()'s own negative return range would otherwise be
 * ambiguous between the two). Returns true if the shell was launched at
 * all (regardless of how it exited). */
static bool try_login_shell(const char *path)
{
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        return false;
    }
    char *const argv[] = { (char *)path, NULL };
    elf_exec_argv(path, 1, argv, shell_build_envp());
    return true;
}

static void run_login_shell(void)
{
    const char *shell_path = shell_getenv("SHELL");
    if (!shell_path || !shell_path[0]) {
        shell_path = "/bin/sh";
    }
    if (try_login_shell(shell_path)) {
        return;
    }
    if (strcmp(shell_path, "/bin/puresh") != 0 && try_login_shell("/bin/puresh")) {
        return;
    }
    printf("warning: could not start a login shell (%s) — falling back to the built-in recovery shell\n",
           shell_path);
    shell_run();
}

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
    tty_init();
    ata_init();
    pci_scan();
    e1000_init();
    xhci_init();
    eth_init();
    arp_init();
    ip_init();
    icmp_init();
    /* No DHCP yet (a later phase) -- 10.0.2.15/24 via 10.0.2.2 matches what
     * QEMU's "-netdev user" (SLIRP) backend hands out by DHCP, so this host
     * is reachable from (and can reach) that same virtual network without
     * needing a real DHCP client. */
    ip_configure(IP4_ADDR(10, 0, 2, 15), IP4_ADDR(255, 255, 255, 0), IP4_ADDR(10, 0, 2, 2));
    vfs_init();

    /* fat.img/root.img travel as GRUB modules baked into build/pureunix.iso
     * (see boot/grub.cfg and the Makefile's $(ISO) rule) so the ISO is a
     * standalone, self-contained boot image — no separate IDE drives
     * needed. ramdisk_attach() wraps each module's identity-mapped memory
     * (see kernel/vmm.c) as a disk_device_t. Falls back to real ATA disks
     * (ata0/ata1) when no matching module was loaded — e.g. hand-built
     * QEMU invocations, or real IDE hardware without a modules-capable
     * bootloader. */
    disk_device_t *fat_disk = NULL;
    disk_device_t *root_disk = NULL;
    for (uint32_t i = 0; i < pmm_module_count(); ++i) {
        const boot_module_t *mod = pmm_module_get(i);
        if (strcmp(mod->cmdline, "fat.img") == 0) {
            fat_disk = ramdisk_attach(0, (uint8_t *)mod->start, mod->end - mod->start);
        } else if (strcmp(mod->cmdline, "root.img") == 0) {
            root_disk = ramdisk_attach(1, (uint8_t *)mod->start, mod->end - mod->start);
        }
    }

    /* FAT16 — compatibility/testing only, mounted at /fat */
    disk_device_t *disk = fat_disk ? fat_disk : ata_primary_master();
    if (disk->present && fat16_mount(disk) == 0) {
        vfs_mount("/fat", VFS_FS_FAT16, fat16_vfs_ops(), NULL);
        printf("FAT16 mounted on /fat\n");
    } else {
        printf("No FAT16 disk found; /fat will be unavailable.\n");
    }

    /* EXT2 — primary root filesystem */
    disk_device_t *disk2 = root_disk ? root_disk : ata_primary_slave();
    if (disk2->present) {
        if (ext2_mount(disk2) == 0) {
            vfs_mount("/", VFS_FS_EXT2, ext2_vfs_ops(), NULL);
            printf("EXT2 mounted on /\n");
        } else {
            printf("EXT2 mount failed on %s; root filesystem unavailable.\n", disk2->name);
        }
    }

    arch_enable_interrupts();

    xhci_enumerate();

    e1000_selftest();
    arp_selftest();
    icmp_selftest();

    crypto_init();
    if (crypto_ready()) {
        printf("Crypto OK\n");
    } else {
        /* Every login is verified cryptographically (see kernel/users.c) —
         * without a working CoreCrypto there is no safe way to check a
         * password, so refuse to boot into a login prompt at all. */
        panic("CoreCrypto self-test failed; refusing to start login.");
    }

    if (users_first_boot()) {
        users_first_boot_setup();
    }

    for (;;) {
        users_login();
        run_login_shell();
    }
}
