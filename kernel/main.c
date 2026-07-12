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
#include <pureunix/mbr.h>
#include <pureunix/memory.h>
#include <pureunix/panic.h>
#include <pureunix/pci.h>
#include <pureunix/procfs.h>
#include <pureunix/ramdisk.h>
#include <pureunix/serial.h>
#include <pureunix/shell.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>
#include <pureunix/tty.h>
#include <pureunix/usb_msd.h>
#include <pureunix/vfs.h>
#include <pureunix/vga.h>
#include <pureunix/vt.h>
#include <pureunix/xhci.h>

/* The login shell for an interactive session — BusyBox ash by default
 * (set via SHELL="/bin/sh" at boot), launched as a genuine ring-3 process
 * via elf_exec_argv(). Blocks until the shell exits, then kernel_main()'s
 * own for(;;) loop restarts it immediately — no login prompt between
 * sessions, just a fresh shell.
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

/* The dedicated orphan-adoption task (task_set_init_pid(), kernel/task.c) —
 * not literally PID 1 (main_task, tasking_init(), already holds that,
 * running VT1's session), so this ends up as PID 2 instead: a small,
 * honest deviation from strict POSIX PID-1 numbering, not a functional
 * gap. Does nothing but continuously reap whatever child task_exit() has
 * re-parented to it, guaranteeing no process is ever a permanent zombie
 * just because its real parent exited before it did. See
 * docs/process-management.md. */
static void init_reaper_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (task_waitpid(-1, NULL, 0) < 0) {
            /* No children at all right now (the common case — orphans
             * are the exception, not the rule) — task_waitpid() returns
             * immediately rather than blocking when it has zero children,
             * so poll instead of busy-looping. Once it *does* have at
             * least one live child, task_waitpid() blocks (cooperatively)
             * on its own until that child exits, no extra sleep needed. */
            pit_sleep(50);
        }
    }
}

/* Entry point for VT2..NUM_VTS's session tasks (kernel/vt.c, task_create()
 * below) — VT1 keeps running as main_task itself (see kernel_main()'s tail
 * loop) rather than getting a seventh task, since kernel_main() already is
 * a task in tasking_init()'s sense.
 *
 * task_alloc() (kernel/task.c) copies vt_id from the *creator* at creation
 * time, same as uid/gid/cwd — which would make every one of these inherit
 * main_task's own vt_id (0, VT1) rather than the one it's actually meant
 * for. Overriding it here, as the very first thing this task does once
 * it's actually scheduled (task_current() now correctly returns this task,
 * not its creator), is what makes it — and, via that same inheritance
 * rule, every process it goes on to launch (its shell, and everything the
 * shell launches) — belong to its own VT instead. */
static void vt_session_main(void *arg)
{
    int vt_id = (int)(uint32_t)arg;
    task_current()->vt_id = vt_id;
    /* Every VT's session task is its own session leader and initial
     * controlling-terminal owner — see vt_claim_session() and
     * docs/process-management.md. */
    vt_claim_session(vt_id);
    for (;;) {
        run_login_shell();
    }
}

/* Boot-checkpoint diagnostic: total RAM the PMM is managing plus a full
 * heap_dump() (bounds, free/used/largest-free-block, cumulative allocation
 * failures, integrity verdict) -- see heap_dump()'s comment in
 * include/pureunix/memory.h. Sprinkled after the boot stages most likely to
 * eat a disproportionate chunk of the fixed-size kernel heap (framebuffer
 * shadow, xHCI DMA setup, filesystem mount, login) so a later "kcalloc
 * failed" can be traced back to which stage actually exhausted it, instead
 * of wrongly being read as "this machine is out of RAM." */
static void boot_checkpoint(const char *label)
{
    printf("checkpoint[%s]: ram_total=%u KiB ram_free=%u KiB\n", label, pmm_total_memory_kb(),
           pmm_free_memory_kb());
    heap_dump(label);
}

/* Persistent-root search: a real disk (a flashed USB stick -- via
 * drivers/usb_msd.c, enumerated by xhci_enumerate() just above this call in
 * kernel_main() -- or a real/emulated ATA/IDE disk, or in QEMU a plain
 * `-drive`) carries a real MBR partition table written at build time
 * (tools/mkdiskimg.py) with one Linux-native (0x83) partition holding the
 * EXT2 root filesystem, GRUB having booted straight off it with no ramdisk
 * modules declared. Tries ATA master/slave then every attached USB Mass
 * Storage device; a trial ext2_mount() (cheap: just a superblock+BGDT read,
 * non-fatal on bad magic) confirms the partition is really EXT2 rather than
 * some other 0x83 use. Returns NULL if nothing has a matching, mountable
 * partition — kernel_main() falls back to the pre-existing
 * ata_primary_slave() whole-disk behavior in that case, so the hand-built
 * two-raw-disk dev/test workflow (fat.img on master, unpartitioned
 * ext2.img on slave — no MBR signature, so this probe always and safely
 * misses) is completely unaffected. */
static disk_device_t *find_persistent_root_disk(void)
{
    disk_device_t *candidates[2 + USB_MSD_MAX_DEVICES];
    int n = 0;
    candidates[n++] = ata_primary_master();
    candidates[n++] = ata_primary_slave();
    for (int i = 0; i < USB_MSD_MAX_DEVICES; ++i) {
        disk_device_t *usb_disk = usb_msd_disk(i);
        if (usb_disk) {
            candidates[n++] = usb_disk;
        }
    }

    for (int i = 0; i < n; ++i) {
        uint32_t start_lba, sector_count;
        if (!mbr_find_partition(candidates[i], MBR_PART_TYPE_LINUX, &start_lba, &sector_count)) {
            continue;
        }

        disk_device_t *part = mbr_partition_disk(candidates[i], start_lba, sector_count);
        if (ext2_mount(part) == 0) {
            printf("Root partition found on %s (LBA %u, %u sectors)\n",
                   candidates[i]->name, (unsigned)start_lba, (unsigned)sector_count);
            return part;
        }
        /* Wrong contents for a 0x83 partition on this disk (bad EXT2 magic,
         * etc.) — ext2_super_read() already printed why; keep looking. */
    }

    return NULL;
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
    /* Allocates its own PMM frames, not heap memory (see fb_enable_shadow()'s
     * comment in drivers/framebuffer.c for why, and for why it must run
     * before anything else in this function ever calls pmm_alloc_frame()) --
     * needed for the console to scroll at a usable speed on real hardware. */
    fb_enable_shadow();
    boot_checkpoint("after framebuffer init");
    tasking_init();
    syscall_init();
    pit_init(100);
    /* Spawned as early as possible (right after the PIT it relies on for
     * its own idle-poll sleep is up) so no real process can possibly
     * exit — and need an orphan-adoption target — before this exists.
     * Naturally becomes PID 2, right after main_task's PID 1. */
    task_t *init_reaper = task_create("init-reaper", init_reaper_main, NULL);
    if (init_reaper) {
        task_set_init_pid(init_reaper->id);
    }
    keyboard_init();
    tty_init();
    vt_init();
    /* main_task (this function's own task, per tasking_init()) becomes
     * VT1's session for the rest of boot -- see the tail loop below and
     * vt_session_main() above for VT2..NUM_VTS. */
    task_current()->vt_id = 0;
    vt_claim_session(0);
    ata_init();
    pci_scan();
    e1000_init();
    xhci_init();
    boot_checkpoint("after xhci init");

    /* Enabled here (rather than just before the network self-tests, where
     * this used to live) and xhci_enumerate() run immediately after, so
     * any USB Mass Storage disk is registered *before* the EXT2 root
     * search below runs — needed so find_persistent_root_disk() can find
     * a real, flashed USB stick, not just ata_primary_master/slave().
     * Everything below here already ran fine with interrupts enabled in
     * the old ordering (network self-tests, right after this originally),
     * so moving the toggle earlier changes nothing about what those
     * relied on — xHCI/e1000's own interrupt handlers are already
     * registered by this point (xhci_init()/e1000_init(), just above). */
    arch_enable_interrupts();
    xhci_enumerate();

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

    /* EXT2 — primary root filesystem. Priority: (1) a GRUB ramdisk module
     * (live/ISO boot, unchanged); (2) a real disk carrying a partitioned,
     * persistent EXT2 root (find_persistent_root_disk() — the flashed-USB
     * / installed-disk case this boot path exists for); (3) the pre-
     * existing unpartitioned ata_primary_slave() fallback, byte-for-byte
     * unchanged, for the hand-built two-raw-disk dev/test QEMU workflow. */
    if (root_disk) {
        if (ext2_mount(root_disk) == 0) {
            vfs_mount("/", VFS_FS_EXT2, ext2_vfs_ops(), NULL);
            printf("EXT2 mounted on /\n");
        } else {
            printf("EXT2 mount failed on %s; root filesystem unavailable.\n", root_disk->name);
        }
    } else if (find_persistent_root_disk()) {
        /* find_persistent_root_disk() already performed the real mount as
         * part of its own trial-mount probe — just register it. */
        vfs_mount("/", VFS_FS_EXT2, ext2_vfs_ops(), NULL);
        printf("EXT2 mounted on /\n");
    } else {
        disk_device_t *disk2 = ata_primary_slave();
        if (disk2->present) {
            if (ext2_mount(disk2) == 0) {
                vfs_mount("/", VFS_FS_EXT2, ext2_vfs_ops(), NULL);
                printf("EXT2 mounted on /\n");
            } else {
                printf("EXT2 mount failed on %s; root filesystem unavailable.\n", disk2->name);
            }
        }
    }

    /* /proc — synthetic, read-only, generated on demand from
     * kernel/task.c's own live task list (fs/procfs.c). Mounted after
     * tasking_init() (already true — this runs well after it) so
     * task_list() always has at least main_task to report. */
    vfs_mount("/proc", VFS_FS_PROCFS, procfs_vfs_ops(), NULL);
    printf("procfs mounted on /proc\n");

    boot_checkpoint("after filesystem mount");

    e1000_selftest();
    arp_selftest();
    icmp_selftest();

    crypto_init();
    if (crypto_ready()) {
        printf("Crypto OK\n");
    } else {
        printf("Warning: CoreCrypto self-test failed — continuing without password auth\n");
    }

    /* Auto-login as root — no first-boot wizard, no login prompt.
     * BusyBox ash starts immediately on every boot. */
    task_set_creds(0, 0);
    shell_setenv("USER", "root");
    shell_setenv("HOME", "/root");
    shell_setenv("SHELL", "/bin/sh");
    /* Every VT's console speaks the same "pureunix" terminfo entry
     * (docs/ncurses-port.md, third_party/ncurses/pureunix.terminfo) —
     * without this, ncurses' initscr() has no TERM to resolve at all and
     * fails outright ("Error opening terminal: unknown."). One global env
     * table (shell/builtins.c) shared by every VT session task below, so
     * setting this once here covers VT1..NUM_VTS, same as USER/HOME/SHELL
     * above. */
    shell_setenv("TERM", "pureunix");
    shell_set_home_cwd("/root");
    boot_checkpoint("after auto-login");

    for (int i = 1; i < NUM_VTS; ++i) {
        if (!task_create("vt-session", vt_session_main, (void *)(uint32_t)i)) {
            printf("warning: failed to start VT%d session\n", i + 1);
        }
    }

    for (;;) {
        run_login_shell();
    }
}
