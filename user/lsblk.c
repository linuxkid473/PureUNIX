#include "libpure.h"

/* Minimal `lsblk` equivalent -- BusyBox has no lsblk applet at all (it's
 * util-linux, not BusyBox), so this is a small standalone program reading
 * SYS_DISK_LIST directly, mirroring user/hello.c's shape. Lists every
 * registered disk and partition (whole disks and their partitions appear
 * as separate rows, exactly as pu_disk_list() reports them -- there is no
 * tree/indent relationship tracked anywhere to nest them visually). */

static void print_uint(unsigned int v)
{
    pu_puti((int)v);
}

static const char *kind_label(unsigned int kind)
{
    switch (kind) {
    case 1: return "ATA";
    case 2: return "USB";
    case 3: return "part";
    case 4: return "RAM";
    default: return "?";
    }
}

static unsigned int name_len(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

/* Integer-only "N MB"/"N.D GB" -- no float formatting in this runtime. */
static void print_size(unsigned int sector_count)
{
    unsigned int mib = sector_count / 2048U;
    if (mib >= 1024U) {
        print_uint(mib / 1024U);
        pu_puts(".");
        print_uint((mib % 1024U) * 10U / 1024U);
        pu_puts("G");
    } else {
        print_uint(mib);
        pu_puts("M");
    }
}

int main(void)
{
    pu_disk_info_t infos[32];
    int n = pu_disk_list(infos, 32);
    if (n <= 0) {
        pu_puts("NAME  KIND SIZE RM\n");
        return 0;
    }
    if (n > 32) n = 32;

    pu_puts("NAME  KIND SIZE RM\n");
    for (int i = 0; i < n; ++i) {
        if (!infos[i].present) continue;
        pu_puts(infos[i].name);
        unsigned int len = name_len(infos[i].name);
        for (unsigned int p = len; p < 6; ++p) pu_puts(" ");
        pu_puts(kind_label(infos[i].kind));
        pu_puts(" ");
        print_size(infos[i].sector_count);
        pu_puts(" ");
        pu_puts(infos[i].removable ? "1" : "0");
        pu_puts("\n");
    }
    return 0;
}
