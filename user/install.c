#include "libpure.h"

/* PureUnix installer: partitions a target disk, formats it EXT2, copies
 * the live system onto it, installs a real GRUB BIOS bootloader, and makes
 * the disk independently bootable and persistent. See docs/install.md.
 *
 * A bare libpure program (like user/hello.c/user/ping.c), not newlib-linked
 * -- everything it needs (disk/mount/mkfs/partition syscalls, plus the
 * existing open/read/write/mkdir/readdir/symlink/chmod file syscalls for
 * the recursive copy) is already exposed there, and BusyBox's own build
 * process has no clean way to absorb a custom non-upstream applet (see
 * tools/build-busybox.sh), so a standalone ELF on $PATH is the right
 * integration point -- ash finds it exactly like any other command. */

/* First partition always starts at LBA 2048 (1 MiB alignment) -- the
 * universal modern convention (also what fdisk/parted default to), chosen
 * specifically so the "embedding gap" between the MBR and the first
 * partition (LBA 1..2047, ~1023 KiB) comfortably fits GRUB's core.img
 * (~100 KiB) -- the legacy LBA-63 convention only leaves ~31 KiB, a real
 * historical source of "core.img doesn't fit" failures in other
 * installers. */
#define TARGET_START_LBA 2048U
#define SECTOR_SIZE 512U

static void print_uint(unsigned int v)
{
    pu_puti((int)v);
}

/* Standard 8-4-4-4-12 hex UUID string ("xxxxxxxx-xxxx-xxxx-xxxx-
 * xxxxxxxxxxxx", 36 chars + NUL) -- embedded into the target's grub.cfg as
 * `pureunix.root_uuid=` and printed to the console so it's easy to
 * cross-check against what kernel_main() reports at boot. */
static void format_uuid(const unsigned char uuid[16], char *out)
{
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[(uuid[i] >> 4) & 0xF];
        out[o++] = hex[uuid[i] & 0xF];
    }
    out[o] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_cpy(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void join_path(char *out, int outsize, const char *dir, const char *name)
{
    int i = 0;
    for (; dir[i] && i < outsize - 1; ++i) out[i] = dir[i];
    if (i > 0 && out[i - 1] != '/' && i < outsize - 1) out[i++] = '/';
    int j = 0;
    for (; name[j] && i < outsize - 1; ++i, ++j) out[i] = name[j];
    out[i] = '\0';
}

/* Reads one line of input (the console tty already does canonical line
 * editing/echo -- see user/termiostest.c's demo_read_line() for the same
 * pattern), trimming the trailing newline. */
static void read_line(char *buf, int cap)
{
    int n = pu_read(0, buf, cap - 1);
    if (n < 0) n = 0;
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
}

/* Integer-only "N MB"/"N.D GB" formatting -- no float formatting available
 * in this bare runtime. 2048 sectors/MiB (512-byte sectors). */
static void print_size(unsigned int sector_count)
{
    unsigned int mib = sector_count / 2048U;
    if (mib >= 1024U) {
        print_uint(mib / 1024U);
        pu_puts(".");
        print_uint((mib % 1024U) * 10U / 1024U);
        pu_puts(" GB");
    } else {
        print_uint(mib);
        pu_puts(" MB");
    }
}

static const char *kind_label(unsigned int kind)
{
    switch (kind) {
    case 1: return "ATA";
    case 2: return "USB";
    case 3: return "Partition";
    case 4: return "RAM";
    default: return "Disk";
    }
}

static void copy_file(const char *src, const char *dst, unsigned int mode)
{
    int in = pu_open(src, O_RDONLY);
    if (in < 0) return;
    int out = pu_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        pu_close(in);
        return;
    }
    char buf[4096];
    int n;
    while ((n = pu_read(in, buf, sizeof(buf))) > 0) {
        pu_write(out, buf, n);
    }
    pu_close(in);
    pu_close(out);
    pu_chmod(dst, mode & 07777U);
}

/* Recursively copies src_dir onto dst_dir -- both real, already-mounted
 * VFS directories (a source and destination path, not disk names). Never
 * needs to special-case virtual mountpoints like /proc or /fat: neither
 * has a real directory entry in EXT2's own root (confirmed: vfs_mount()
 * never creates one), so plain readdir("/") never surfaces them at all --
 * this walk only ever sees what's genuinely on disk.
 *
 * `entries[128]` (~9 KiB: PU_MAX_NAME=64-byte names + two ints, times 128)
 * must NOT live in copy_tree()'s own stack frame -- recursing into a real
 * directory tree (the TCC sysroot's /usr/include nests several levels
 * deep) blew a real 64 KiB task stack (kernel/task.c's TASK_STACK_SIZE)
 * after only ~6-7 levels when it did, crashing with a page fault (found
 * running this installer against the live system's own real userland, not
 * a synthetic test). Indexed by recursion depth in static storage instead
 * -- bounded, and each depth's own array stays valid across the recursive
 * call into a subdirectory (unlike a single shared static array, which a
 * naive fix would have overwritten out from under the caller's own
 * still-in-progress loop). MAX_COPY_DEPTH is generous headroom over any
 * real path depth this installer ever copies. */
#define MAX_COPY_DEPTH 32
static struct dirent g_copy_entries[MAX_COPY_DEPTH][128];

/* One dot per file/directory copied -- visible liveness during a copy that
 * can take real minutes over USB, without the noise of a full path dump. */
static int g_copy_progress_count;

static void copy_tree(const char *src_dir, const char *dst_dir, int depth)
{
    if (depth >= MAX_COPY_DEPTH) {
        pu_puts("  (skipping "); pu_puts(src_dir); pu_puts(": too deeply nested)\n");
        return;
    }

    struct dirent *entries = g_copy_entries[depth];
    int n = pu_readdir(src_dir, entries, 128);
    if (n < 0) return;
    if (n > 128) n = 128;

    for (int i = 0; i < n; ++i) {
        if (str_eq(entries[i].name, ".") || str_eq(entries[i].name, "..")) continue;

        if (++g_copy_progress_count % 8 == 0) pu_puts(".");

        char src_path[256], dst_path[256];
        join_path(src_path, sizeof(src_path), src_dir, entries[i].name);
        join_path(dst_path, sizeof(dst_path), dst_dir, entries[i].name);

        struct stat st;
        if (pu_lstat(src_path, &st) != 0) continue;

        if (st.st_type == 3) {
            /* Symlink: recreate the link itself, never follow it. */
            char target[256];
            int tn = pu_readlink(src_path, target, sizeof(target) - 1);
            if (tn >= 0) {
                target[tn] = '\0';
                pu_symlink(target, dst_path);
            }
        } else if (st.st_type == 2) {
            pu_mkdir(dst_path);
            copy_tree(src_path, dst_path, depth + 1);
            pu_chmod(dst_path, st.st_mode & 07777U);
        } else if (S_ISCHR(st.st_mode)) {
            /* Cosmetic device node (/dev/ttyN, /dev/tty -- see mkext2.py's
             * add_dev() comment: PureUNIX has no real device-node/rdev
             * machinery, so these are just empty files with S_IFCHR mode
             * bits, existing only so the paths are real and listable).
             * SYS_OPEN specially intercepts these exact paths and binds
             * the resulting fd straight to a live VT's keyboard queue
             * instead of ever reading their (empty) on-disk content --
             * opening one here the same way copy_file() opens a regular
             * file would block forever waiting for a keypress that never
             * comes. Recreate it the same way mkext2.py originally built
             * it: an empty file with the same mode, no read needed. */
            int fd = pu_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd >= 0) pu_close(fd);
            pu_chmod(dst_path, st.st_mode & 07777U);
        } else {
            copy_file(src_path, dst_path, st.st_mode);
        }
    }
}

/* Splices GRUB's boot.img bootstrap code (bytes 0-445) onto the target
 * disk's LBA 0, while preserving whatever is already at bytes 446-511 --
 * the partition table pu_disk_partition() just wrote there, plus its
 * 0x55AA signature. Must never touch those bytes: this is the single
 * highest real-disk-corruption risk in the entire installer -- writing the
 * wrong 512 bytes to LBA 0 destroys the partition table outright. Mirrors
 * exactly what a real `grub-bios-setup` does internally. */
static int install_boot_sector(const char *disk_name, unsigned int confirm_sector_count)
{
    unsigned char current[SECTOR_SIZE];
    if (pu_disk_read(disk_name, 0, current) != 0) return -1;

    int fd = pu_open("/boot/grub/i386-pc/boot.img", O_RDONLY);
    if (fd < 0) return -1;
    unsigned char boot_img[SECTOR_SIZE];
    int n = pu_read(fd, (char *)boot_img, sizeof(boot_img));
    pu_close(fd);
    if (n != (int)SECTOR_SIZE) return -1;

    /* boot.img[0..445] (bootstrap code) overlays current[0..445]; bytes
     * 446-511 (partition table + boot signature) come from `current`,
     * completely untouched. */
    unsigned char spliced[SECTOR_SIZE];
    for (unsigned int i = 0; i < 446U; ++i) spliced[i] = boot_img[i];
    for (unsigned int i = 446U; i < SECTOR_SIZE; ++i) spliced[i] = current[i];

    return pu_disk_write(disk_name, 0, confirm_sector_count, spliced);
}

/* Writes GRUB's core.img starting at LBA 1, one sector at a time (no need
 * to buffer the whole ~100 KiB file -- this bare runtime has no malloc()),
 * padding the final partial sector with zeros. The embedding gap (LBA
 * 1..2047, ~1023 KiB) comfortably fits core.img; this never checks for
 * overrun into the first partition (start_lba is always
 * TARGET_START_LBA=2048, generous headroom over any realistic core.img
 * size for this module set). */
static int install_core_image(const char *disk_name, unsigned int confirm_sector_count)
{
    int fd = pu_open("/boot/grub/i386-pc/core.img", O_RDONLY);
    if (fd < 0) return -1;

    unsigned int lba = 1;
    unsigned char sector[SECTOR_SIZE];
    int n;
    while ((n = pu_read(fd, (char *)sector, SECTOR_SIZE)) > 0) {
        if (n < (int)SECTOR_SIZE) {
            for (unsigned int i = (unsigned int)n; i < SECTOR_SIZE; ++i) sector[i] = 0;
        }
        if (pu_disk_write(disk_name, lba, confirm_sector_count, sector) != 0) {
            pu_close(fd);
            return -1;
        }
        lba++;
    }
    pu_close(fd);
    return 0;
}

int main(void)
{
    pu_puts("PureUnix Installer\n");
    pu_puts("==================\n\n");
    pu_puts("Detecting disks...\n");

    pu_disk_info_t infos[16];
    int n = pu_disk_list(infos, 16);
    if (n <= 0) {
        pu_puts("No disks found.\n");
        return 1;
    }
    if (n > 16) n = 16;

    pu_puts("\nAvailable disks:\n\n");
    for (int i = 0; i < n; ++i) {
        pu_puts("  ");
        print_uint((unsigned int)(i + 1));
        pu_puts(") ");
        pu_puts(kind_label(infos[i].kind));
        pu_puts("\t");
        pu_puts(infos[i].name);
        pu_puts("\t");
        print_size(infos[i].sector_count);
        pu_puts("\n");
    }

    pu_puts("\nSelect installation target (number): ");
    char line[32];
    read_line(line, sizeof(line));
    int choice = pu_atoi(line);
    if (choice < 1 || choice > n) {
        pu_puts("Invalid selection.\n");
        return 1;
    }
    pu_disk_info_t *target = &infos[choice - 1];

    if (target->kind == 3) {
        pu_puts("Please select a whole disk, not an existing partition -- the installer "
                "creates its own partition on the disk you choose.\n");
        return 1;
    }

    pu_puts("\nWARNING:\n");
    pu_puts("Installing PureUnix will erase all data on this drive.\n\n");
    pu_puts("  ");
    pu_puts(target->name);
    pu_puts(" (");
    print_size(target->sector_count);
    pu_puts(")\n\n");
    pu_puts("Continue? (yes/no) ");
    read_line(line, sizeof(line));
    if (!str_eq(line, "yes")) {
        pu_puts("Aborted.\n");
        return 1;
    }

    if (target->sector_count <= TARGET_START_LBA) {
        pu_puts("Disk too small to install onto.\n");
        return 1;
    }

    pu_puts("\nPartitioning...\n");
    pu_partition_entry_t entry;
    entry.type = 0x83;
    entry.bootable = 1;
    entry.start_lba = TARGET_START_LBA;
    entry.sector_count = target->sector_count - TARGET_START_LBA;
    if (pu_disk_partition(target->name, target->sector_count, &entry, 1) != 0) {
        pu_puts("Partitioning failed.\n");
        return 1;
    }

    char part_name[20];
    str_cpy(part_name, target->name);
    size_t l = str_len(part_name);
    part_name[l] = '1';
    part_name[l + 1] = '\0';

    pu_puts("Creating filesystem...\n");
    unsigned char root_uuid[16];
    if (pu_mkfs_ext2(part_name, 0, entry.sector_count, "PUREUNIX_ROOT", root_uuid) != 0) {
        pu_puts("Filesystem creation failed.\n");
        return 1;
    }

    if (pu_mount(part_name, "/mnt") != 0) {
        pu_puts("Mount failed.\n");
        return 1;
    }

    pu_puts("Installing kernel and userspace...\n");
    copy_tree("/", "/mnt", 0);
    pu_puts("\n");

    pu_puts("Installing bootloader...\n");
    if (install_boot_sector(target->name, target->sector_count) != 0) {
        pu_puts("Warning: writing the boot sector failed.\n");
    }
    if (install_core_image(target->name, target->sector_count) != 0) {
        pu_puts("Warning: writing the GRUB core image failed.\n");
    }

    /* The real boot menu -- core.img's embedded prefix (boot/grub-
     * embedded.cfg) just searches for this by label and hands off here.
     * No ramdisk modules: kernel_main() falls back to searching the disk/
     * partition registry for the PUREUNIX_ROOT label whenever none are
     * present (Milestone 9), which is exactly this installed disk.
     *
     * `pureunix.root_uuid=<uuid>` on the multiboot2 command line is what
     * makes root selection on THIS disk unambiguous: if more than one
     * PUREUNIX_ROOT-labeled disk is attached at boot (e.g. an earlier test
     * install left on another drive), the label alone can't tell them
     * apart -- kernel_main() reads this UUID back off the command line and
     * requires an exact match, refusing to silently guess. */
    {
        char uuid_str[37];
        format_uuid(root_uuid, uuid_str);
        pu_puts("  root UUID: "); pu_puts(uuid_str); pu_puts("\n");

        int cfgfd = pu_open("/mnt/boot/grub/grub.cfg", O_WRONLY | O_CREAT | O_TRUNC);
        if (cfgfd >= 0) {
            static const char cfg_head[] =
                "set timeout=1\n"
                "set default=0\n"
                "menuentry \"PureUnix\" {\n"
                "    multiboot2 /boot/pureunix.elf pureunix.root_uuid=";
            static const char cfg_tail[] = "\n    boot\n}\n";
            pu_write(cfgfd, cfg_head, str_len(cfg_head));
            pu_write(cfgfd, uuid_str, str_len(uuid_str));
            pu_write(cfgfd, cfg_tail, str_len(cfg_tail));
            pu_close(cfgfd);
        }
    }

    pu_puts("Syncing...\n");
    pu_sync();
    pu_umount("/mnt");

    pu_puts("\nInstallation complete.\n\n");
    pu_puts("Remove the installation media and reboot.\n");
    return 0;
}
