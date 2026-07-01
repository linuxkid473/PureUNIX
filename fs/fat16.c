#include <pureunix/ctype.h>
#include <pureunix/fat16.h>
#include <pureunix/memory.h>
#include <pureunix/stat.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F
#define FAT_EOC            0xFFF8
#define FAT_FREE           0x0000

typedef struct fat_bpb {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors16;
    uint8_t media;
    uint16_t sectors_per_fat16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed)) fat_bpb_t;

typedef struct fat_dir_entry {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

typedef struct fat16_fs {
    disk_device_t *disk;
    bool mounted;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t root_dir_sector;
    uint32_t first_data_sector;
    uint32_t max_cluster;
} fat16_fs_t;

typedef struct found_entry {
    fat_dir_entry_t entry;
    uint32_t dir_cluster;
    uint32_t index;
    bool found;
} found_entry_t;

static fat16_fs_t fs;

static uint32_t cluster_size(void)
{
    return fs.bytes_per_sector * fs.sectors_per_cluster;
}

static uint32_t entry_first_cluster(const fat_dir_entry_t *entry)
{
    return ((uint32_t)entry->first_cluster_high << 16) | entry->first_cluster_low;
}

static void entry_set_first_cluster(fat_dir_entry_t *entry, uint32_t cluster)
{
    entry->first_cluster_low = (uint16_t)(cluster & 0xFFFF);
    entry->first_cluster_high = (uint16_t)(cluster >> 16);
}

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return fs.first_data_sector + (cluster - 2) * fs.sectors_per_cluster;
}

static int disk_read(uint32_t lba, uint8_t *buf)
{
    return fs.disk && fs.disk->read ? fs.disk->read(lba, buf) : -1;
}

static int disk_write(uint32_t lba, const uint8_t *buf)
{
    return fs.disk && fs.disk->write ? fs.disk->write(lba, buf) : -1;
}

static int fat_read(uint32_t cluster, uint16_t *value)
{
    uint8_t sector[512];
    uint32_t offset = cluster * 2;
    uint32_t sector_lba = fs.first_fat_sector + offset / fs.bytes_per_sector;
    uint32_t sector_off = offset % fs.bytes_per_sector;
    if (disk_read(sector_lba, sector) != 0) {
        return -1;
    }
    *value = sector[sector_off] | ((uint16_t)sector[sector_off + 1] << 8);
    return 0;
}

static int fat_write(uint32_t cluster, uint16_t value)
{
    uint8_t sector[512];
    uint32_t offset = cluster * 2;
    uint32_t sector_off = offset % fs.bytes_per_sector;

    for (uint8_t fat = 0; fat < fs.fat_count; ++fat) {
        uint32_t sector_lba = fs.first_fat_sector + fat * fs.sectors_per_fat + offset / fs.bytes_per_sector;
        if (disk_read(sector_lba, sector) != 0) {
            return -1;
        }
        sector[sector_off] = value & 0xFF;
        sector[sector_off + 1] = value >> 8;
        if (disk_write(sector_lba, sector) != 0) {
            return -1;
        }
    }
    return 0;
}

static int zero_cluster(uint32_t cluster)
{
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint8_t s = 0; s < fs.sectors_per_cluster; ++s) {
        if (disk_write(cluster_to_lba(cluster) + s, zero) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t alloc_cluster(void)
{
    for (uint32_t c = 2; c < fs.max_cluster; ++c) {
        uint16_t value;
        if (fat_read(c, &value) != 0) {
            printf("[fat16] alloc: fat_read failed at cluster %u\n", (unsigned)c);
            return 0;
        }
        if (value == FAT_FREE) {
            if (fat_write(c, 0xFFFF) != 0) {
                printf("[fat16] alloc: fat_write failed at cluster %u\n", (unsigned)c);
                return 0;
            }
            zero_cluster(c);
            return c;
        }
    }
    printf("[fat16] alloc: no free clusters (max=%u)\n", (unsigned)fs.max_cluster);
    return 0;
}

static void free_chain(uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT_EOC) {
        uint16_t next;
        if (fat_read(cluster, &next) != 0) {
            return;
        }
        fat_write(cluster, FAT_FREE);
        if (next >= FAT_EOC || next == 0) {
            break;
        }
        cluster = next;
    }
}

static int read_dir_entry_at(uint32_t dir_cluster, uint32_t index, fat_dir_entry_t *entry)
{
    uint8_t sector[512];
    uint32_t entries_per_sector = fs.bytes_per_sector / sizeof(fat_dir_entry_t);

    if (dir_cluster == 0) {
        if (index >= fs.root_entry_count) {
            return -1;
        }
        uint32_t lba = fs.root_dir_sector + index / entries_per_sector;
        uint32_t off = (index % entries_per_sector) * sizeof(fat_dir_entry_t);
        if (disk_read(lba, sector) != 0) {
            return -1;
        }
        memcpy(entry, sector + off, sizeof(*entry));
        return 0;
    }

    uint32_t entries_per_cluster = cluster_size() / sizeof(fat_dir_entry_t);
    uint32_t cluster_skip = index / entries_per_cluster;
    uint32_t in_cluster = index % entries_per_cluster;
    uint32_t cluster = dir_cluster;
    while (cluster_skip--) {
        uint16_t next;
        if (fat_read(cluster, &next) != 0 || next >= FAT_EOC) {
            return -1;
        }
        cluster = next;
    }
    uint32_t lba = cluster_to_lba(cluster) + (in_cluster * sizeof(fat_dir_entry_t)) / fs.bytes_per_sector;
    uint32_t off = (in_cluster * sizeof(fat_dir_entry_t)) % fs.bytes_per_sector;
    if (disk_read(lba, sector) != 0) {
        return -1;
    }
    memcpy(entry, sector + off, sizeof(*entry));
    return 0;
}

static int write_dir_entry_at(uint32_t dir_cluster, uint32_t index, const fat_dir_entry_t *entry)
{
    uint8_t sector[512];
    uint32_t entries_per_sector = fs.bytes_per_sector / sizeof(fat_dir_entry_t);

    if (dir_cluster == 0) {
        if (index >= fs.root_entry_count) {
            return -1;
        }
        uint32_t lba = fs.root_dir_sector + index / entries_per_sector;
        uint32_t off = (index % entries_per_sector) * sizeof(fat_dir_entry_t);
        if (disk_read(lba, sector) != 0) {
            return -1;
        }
        memcpy(sector + off, entry, sizeof(*entry));
        return disk_write(lba, sector);
    }

    uint32_t entries_per_cluster = cluster_size() / sizeof(fat_dir_entry_t);
    uint32_t cluster_skip = index / entries_per_cluster;
    uint32_t in_cluster = index % entries_per_cluster;
    uint32_t cluster = dir_cluster;
    while (cluster_skip--) {
        uint16_t next;
        if (fat_read(cluster, &next) != 0 || next >= FAT_EOC) {
            return -1;
        }
        cluster = next;
    }
    uint32_t lba = cluster_to_lba(cluster) + (in_cluster * sizeof(fat_dir_entry_t)) / fs.bytes_per_sector;
    uint32_t off = (in_cluster * sizeof(fat_dir_entry_t)) % fs.bytes_per_sector;
    if (disk_read(lba, sector) != 0) {
        return -1;
    }
    memcpy(sector + off, entry, sizeof(*entry));
    return disk_write(lba, sector);
}

static bool entry_is_free(const fat_dir_entry_t *entry)
{
    return entry->name[0] == 0x00 || entry->name[0] == 0xE5;
}

static bool entry_is_visible(const fat_dir_entry_t *entry)
{
    return !entry_is_free(entry) && entry->attr != FAT_ATTR_LFN && !(entry->attr & FAT_ATTR_VOLUME_ID);
}

static int name_to_83(const char *name, uint8_t out[11])
{
    memset(out, ' ', 11);
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return -1;
    }
    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return -1;
    }
    for (size_t i = 0; i < base_len; ++i) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ' ') {
            return -1;
        }
        out[i] = (uint8_t)toupper(c);
    }
    for (size_t i = 0; i < ext_len; ++i) {
        char c = dot[1 + i];
        if (c == '/' || c == '\\' || c == ' ') {
            return -1;
        }
        out[8 + i] = (uint8_t)toupper(c);
    }
    return 0;
}

static void name_from_83(const fat_dir_entry_t *entry, char *out)
{
    size_t p = 0;
    for (int i = 0; i < 8 && entry->name[i] != ' '; ++i) {
        out[p++] = (char)tolower(entry->name[i]);
    }
    bool has_ext = false;
    for (int i = 8; i < 11; ++i) {
        if (entry->name[i] != ' ') {
            has_ext = true;
            break;
        }
    }
    if (has_ext) {
        out[p++] = '.';
        for (int i = 8; i < 11 && entry->name[i] != ' '; ++i) {
            out[p++] = (char)tolower(entry->name[i]);
        }
    }
    out[p] = '\0';
}

static int find_in_dir(uint32_t dir_cluster, const uint8_t fat_name[11], found_entry_t *out)
{
    fat_dir_entry_t entry;
    uint32_t max = dir_cluster == 0 ? fs.root_entry_count : 65536;
    for (uint32_t i = 0; i < max; ++i) {
        if (read_dir_entry_at(dir_cluster, i, &entry) != 0) {
            break;
        }
        if (entry.name[0] == 0x00) {
            break;
        }
        if (entry_is_visible(&entry) && memcmp(entry.name, fat_name, 11) == 0) {
            if (out) {
                out->entry = entry;
                out->dir_cluster = dir_cluster;
                out->index = i;
                out->found = true;
            }
            return 0;
        }
    }
    return -1;
}

static int path_lookup(const char *path, found_entry_t *out)
{
    if (!fs.mounted || !path || path[0] != '/') {
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        memset(out, 0, sizeof(*out));
        out->entry.attr = FAT_ATTR_DIRECTORY;
        out->found = true;
        return 0;
    }

    char tmp[PUREUNIX_MAX_PATH];
    strncpy(tmp, path + 1, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *save = NULL;
    uint32_t dir = 0;
    found_entry_t found;

    for (char *tok = strtok_r(tmp, "/", &save); tok;) {
        uint8_t fat_name[11];
        if (name_to_83(tok, fat_name) != 0 || find_in_dir(dir, fat_name, &found) != 0) {
            return -1;
        }
        char *next_tok = strtok_r(NULL, "/", &save);
        if (!next_tok) {
            if (out) {
                *out = found;
            }
            return 0;
        }
        if (!(found.entry.attr & FAT_ATTR_DIRECTORY)) {
            return -1;
        }
        dir = entry_first_cluster(&found.entry);
        tok = next_tok;
    }
    return -1;
}

static int path_parent(const char *path, found_entry_t *parent, char *leaf)
{
    if (!path || path[0] != '/' || strcmp(path, "/") == 0) {
        return -1;
    }
    char tmp[PUREUNIX_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (!slash) {
        return -1;
    }
    strncpy(leaf, slash + 1, PUREUNIX_MAX_NAME - 1);
    leaf[PUREUNIX_MAX_NAME - 1] = '\0';
    if (slash == tmp) {
        return path_lookup("/", parent);
    }
    *slash = '\0';
    return path_lookup(tmp, parent);
}

static int find_free_slot(uint32_t dir_cluster, uint32_t *out_index)
{
    fat_dir_entry_t entry;
    uint32_t max = dir_cluster == 0 ? fs.root_entry_count : 65536;
    uint32_t entries_per_cluster = cluster_size() / sizeof(fat_dir_entry_t);

    for (uint32_t i = 0; i < max; ++i) {
        if (read_dir_entry_at(dir_cluster, i, &entry) != 0) {
            if (dir_cluster == 0) {
                return -1;
            }
            uint32_t last = dir_cluster;
            uint16_t next;
            while (fat_read(last, &next) == 0 && next < FAT_EOC) {
                last = next;
            }
            uint32_t n = alloc_cluster();
            if (!n) {
                return -1;
            }
            fat_write(last, (uint16_t)n);
            fat_write(n, 0xFFFF);
            *out_index = ((i + entries_per_cluster - 1) / entries_per_cluster) * entries_per_cluster;
            return 0;
        }
        if (entry_is_free(&entry)) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

static int dir_is_empty(uint32_t cluster)
{
    fat_dir_entry_t entry;
    for (uint32_t i = 0; i < 65536; ++i) {
        if (read_dir_entry_at(cluster, i, &entry) != 0 || entry.name[0] == 0x00) {
            return 1;
        }
        if (!entry_is_visible(&entry)) {
            continue;
        }
        char name[16];
        name_from_83(&entry, name);
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            return 0;
        }
    }
    return 1;
}

static int write_new_chain(uint32_t *out_first, const uint8_t *data, size_t size)
{
    *out_first = 0;
    if (size == 0) {
        return 0;
    }
    uint32_t needed = (size + cluster_size() - 1) / cluster_size();
    uint32_t first = 0;
    uint32_t prev = 0;

    for (uint32_t i = 0; i < needed; ++i) {
        uint32_t c = alloc_cluster();
        if (!c) {
            printf("[fat16] chain: alloc_cluster failed at cluster %u of %u\n",
                   (unsigned)i, (unsigned)needed);
            if (first) {
                free_chain(first);
            }
            return -1;
        }
        if (!first) {
            first = c;
        }
        if (prev) {
            fat_write(prev, (uint16_t)c);
        }
        prev = c;
    }
    fat_write(prev, 0xFFFF);

    uint8_t sector[512];
    size_t written = 0;
    uint32_t cluster = first;
    while (cluster >= 2 && cluster < FAT_EOC && written < size) {
        for (uint8_t s = 0; s < fs.sectors_per_cluster; ++s) {
            memset(sector, 0, sizeof(sector));
            size_t remaining = size - written;
            size_t chunk = remaining > fs.bytes_per_sector ? fs.bytes_per_sector : remaining;
            if (chunk) {
                memcpy(sector, data + written, chunk);
                written += chunk;
            }
            if (disk_write(cluster_to_lba(cluster) + s, sector) != 0) {
                printf("[fat16] chain: disk_write failed lba=%u\n",
                       (unsigned)(cluster_to_lba(cluster) + s));
                free_chain(first);
                return -1;
            }
        }
        uint16_t next;
        if (fat_read(cluster, &next) != 0) {
            break;
        }
        cluster = next;
    }
    *out_first = first;
    return 0;
}

int fat16_mount(disk_device_t *disk)
{
    uint8_t sector[512];
    memset(&fs, 0, sizeof(fs));
    fs.disk = disk;
    if (!disk || !disk->present || disk_read(0, sector) != 0) {
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return -1;
    }

    fat_bpb_t *bpb = (fat_bpb_t *)sector;
    if (bpb->bytes_per_sector != 512 || bpb->sectors_per_fat16 == 0) {
        return -1;
    }

    fs.bytes_per_sector = bpb->bytes_per_sector;
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.reserved_sectors = bpb->reserved_sectors;
    fs.fat_count = bpb->fat_count;
    fs.root_entry_count = bpb->root_entry_count;
    fs.sectors_per_fat = bpb->sectors_per_fat16;
    fs.total_sectors = bpb->total_sectors16 ? bpb->total_sectors16 : bpb->total_sectors32;
    fs.root_dir_sectors = ((fs.root_entry_count * 32) + (fs.bytes_per_sector - 1)) / fs.bytes_per_sector;
    fs.first_fat_sector = fs.reserved_sectors;
    fs.root_dir_sector = fs.first_fat_sector + fs.fat_count * fs.sectors_per_fat;
    fs.first_data_sector = fs.root_dir_sector + fs.root_dir_sectors;
    fs.max_cluster = ((fs.total_sectors - fs.first_data_sector) / fs.sectors_per_cluster) + 2;
    fs.mounted = true;

    printf("FAT16: mounted %u KiB, cluster=%u sectors\n",
           fat16_total_bytes() / 1024, fs.sectors_per_cluster);
    return 0;
}

bool fat16_is_mounted(void)
{
    return fs.mounted;
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return dim[month - 1];
}

/* Decode a FAT16 directory-entry date/time pair into Unix epoch seconds
 * (UTC, no timezone handling — FAT stores local time with no offset).
 * date/time == 0 (as in the synthetic root-directory entry) yields 0. */
static uint32_t fat_datetime_to_unix(uint16_t date, uint16_t time)
{
    if (date == 0) {
        return 0;
    }
    int year = 1980 + (date >> 9);
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;
    int hour = (time >> 11) & 0x1F;
    int minute = (time >> 5) & 0x3F;
    int second = (time & 0x1F) * 2;
    if (month < 1) month = 1;
    if (day < 1) day = 1;

    uint32_t days = 0;
    for (int y = 1970; y < year; ++y) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += (uint32_t)days_in_month(year, m);
    }
    days += (uint32_t)(day - 1);

    return days * 86400u + (uint32_t)hour * 3600u + (uint32_t)minute * 60u + (uint32_t)second;
}

int fat16_stat(const char *path, vfs_stat_t *st)
{
    found_entry_t found;
    if (path_lookup(path, &found) != 0) {
        return -1;
    }
    bool is_dir = (found.entry.attr & FAT_ATTR_DIRECTORY) != 0;
    st->type = is_dir ? VFS_DIR : VFS_FILE;
    st->size = found.entry.size;
    st->mode = found.entry.attr;

    /* FAT16 has no Unix permissions/ownership, so these fields are
     * synthesized rather than read from disk: uid/gid are always 0, mode
     * is file-type bits plus a fixed rwxr-xr-x/rw-r--r-- pattern, and
     * nlink is always 1 (FAT has no hardlink concept). Timestamps are the
     * one part of this that IS real, decoded from the on-disk FAT
     * date/time fields; last_access_date has no time component in FAT16. */
    st->st_mode = (mode_t)((is_dir ? S_IFDIR : S_IFREG) | (is_dir ? 0755 : 0644));
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_ino = (ino_t)((found.dir_cluster << 16) | (found.index & 0xFFFF));
    st->st_atime = fat_datetime_to_unix(found.entry.last_access_date, 0);
    st->st_mtime = fat_datetime_to_unix(found.entry.write_date, found.entry.write_time);
    st->st_ctime = fat_datetime_to_unix(found.entry.create_date, found.entry.create_time);
    st->st_blksize = cluster_size();
    st->st_blocks = (found.entry.size + 511) / 512;
    return 0;
}

int fat16_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    found_entry_t found;
    if (path_lookup(path, &found) != 0 || (found.entry.attr & FAT_ATTR_DIRECTORY)) {
        return -1;
    }
    uint32_t size = found.entry.size;
    uint8_t *data = kmalloc(size + 1);
    if (!data) {
        return -1;
    }
    uint32_t cluster = entry_first_cluster(&found.entry);
    uint8_t sector[512];
    size_t read = 0;

    while (cluster >= 2 && cluster < FAT_EOC && read < size) {
        for (uint8_t s = 0; s < fs.sectors_per_cluster && read < size; ++s) {
            if (disk_read(cluster_to_lba(cluster) + s, sector) != 0) {
                kfree(data);
                return -1;
            }
            size_t remaining = size - read;
            size_t chunk = remaining > fs.bytes_per_sector ? fs.bytes_per_sector : remaining;
            memcpy(data + read, sector, chunk);
            read += chunk;
        }
        uint16_t next;
        if (fat_read(cluster, &next) != 0) {
            break;
        }
        cluster = next;
    }
    data[size] = '\0';
    *out_data = data;
    *out_size = size;
    return 0;
}

int fat16_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    found_entry_t found;
    if (path_lookup(path, &found) != 0) {
        printf("[fat16] write: %s not found, creating\n", path);
        if (fat16_create(path, false) != 0) {
            printf("[fat16] write: create failed for %s\n", path);
            return -1;
        }
        if (path_lookup(path, &found) != 0) {
            printf("[fat16] write: lookup after create failed for %s\n", path);
            return -1;
        }
    }
    if (found.entry.attr & FAT_ATTR_DIRECTORY) {
        printf("[fat16] write: %s is a directory\n", path);
        return -1;
    }

    uint8_t *combined = NULL;
    size_t combined_size = size;
    if (flags & VFS_O_APPEND) {
        uint8_t *old = NULL;
        size_t old_size = 0;
        if (fat16_read_file(path, &old, &old_size) == 0) {
            combined_size = old_size + size;
            combined = kmalloc(combined_size);
            if (!combined) {
                kfree(old);
                return -1;
            }
            memcpy(combined, old, old_size);
            memcpy(combined + old_size, data, size);
            kfree(old);
            data = combined;
        }
    }

    uint32_t new_first = 0;
    if (write_new_chain(&new_first, data, combined_size) != 0) {
        printf("[fat16] write: chain write failed for %s (size=%u)\n", path, (uint32_t)combined_size);
        kfree(combined);
        return -1;
    }
    uint32_t old_first = entry_first_cluster(&found.entry);
    if (old_first) {
        free_chain(old_first);
    }
    entry_set_first_cluster(&found.entry, new_first);
    found.entry.size = combined_size;
    found.entry.attr = FAT_ATTR_ARCHIVE;
    int r = write_dir_entry_at(found.dir_cluster, found.index, &found.entry);
    if (r != 0) {
        printf("[fat16] write: dir entry update failed for %s\n", path);
    }
    kfree(combined);
    return r;
}

int fat16_create(const char *path, bool directory)
{
    found_entry_t parent;
    char leaf[PUREUNIX_MAX_NAME];
    uint8_t fat_name[11];
    if (path_parent(path, &parent, leaf) != 0 || name_to_83(leaf, fat_name) != 0) {
        return -1;
    }
    if (!(parent.entry.attr & FAT_ATTR_DIRECTORY)) {
        return -1;
    }
    uint32_t parent_cluster = entry_first_cluster(&parent.entry);
    if (find_in_dir(parent_cluster, fat_name, NULL) == 0) {
        return -1;
    }
    uint32_t index;
    if (find_free_slot(parent_cluster, &index) != 0) {
        return -1;
    }

    fat_dir_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, fat_name, 11);
    entry.attr = directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    if (directory) {
        uint32_t c = alloc_cluster();
        if (!c) {
            return -1;
        }
        entry_set_first_cluster(&entry, c);
        fat_dir_entry_t dot;
        memset(&dot, 0, sizeof(dot));
        memset(dot.name, ' ', 11);
        dot.name[0] = '.';
        dot.attr = FAT_ATTR_DIRECTORY;
        entry_set_first_cluster(&dot, c);
        write_dir_entry_at(c, 0, &dot);

        fat_dir_entry_t dotdot = dot;
        dotdot.name[1] = '.';
        entry_set_first_cluster(&dotdot, parent_cluster);
        write_dir_entry_at(c, 1, &dotdot);
    }
    return write_dir_entry_at(parent_cluster, index, &entry);
}

int fat16_unlink(const char *path, bool directory)
{
    found_entry_t found;
    if (path_lookup(path, &found) != 0 || strcmp(path, "/") == 0) {
        return -1;
    }
    bool is_dir = found.entry.attr & FAT_ATTR_DIRECTORY;
    if (directory != is_dir) {
        return -1;
    }
    uint32_t first = entry_first_cluster(&found.entry);
    if (is_dir && !dir_is_empty(first)) {
        return -1;
    }
    if (first) {
        free_chain(first);
    }
    found.entry.name[0] = 0xE5;
    return write_dir_entry_at(found.dir_cluster, found.index, &found.entry);
}

int fat16_rename(const char *old_path, const char *new_path)
{
    found_entry_t old_entry;
    found_entry_t parent;
    char leaf[PUREUNIX_MAX_NAME];
    uint8_t fat_name[11];
    if (path_lookup(old_path, &old_entry) != 0 ||
        path_parent(new_path, &parent, leaf) != 0 ||
        name_to_83(leaf, fat_name) != 0) {
        return -1;
    }
    uint32_t parent_cluster = entry_first_cluster(&parent.entry);
    if (find_in_dir(parent_cluster, fat_name, NULL) == 0) {
        return -1;
    }
    uint32_t new_index;
    if (find_free_slot(parent_cluster, &new_index) != 0) {
        return -1;
    }
    fat_dir_entry_t moved = old_entry.entry;
    memcpy(moved.name, fat_name, 11);
    if (write_dir_entry_at(parent_cluster, new_index, &moved) != 0) {
        return -1;
    }
    old_entry.entry.name[0] = 0xE5;
    return write_dir_entry_at(old_entry.dir_cluster, old_entry.index, &old_entry.entry);
}

int fat16_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    found_entry_t found;
    if (path_lookup(path, &found) != 0 || !(found.entry.attr & FAT_ATTR_DIRECTORY)) {
        return -1;
    }
    uint32_t dir_cluster = entry_first_cluster(&found.entry);
    fat_dir_entry_t entry;
    uint32_t max = dir_cluster == 0 ? fs.root_entry_count : 65536;
    for (uint32_t i = 0; i < max; ++i) {
        if (read_dir_entry_at(dir_cluster, i, &entry) != 0 || entry.name[0] == 0x00) {
            break;
        }
        if (!entry_is_visible(&entry)) {
            continue;
        }
        vfs_dirent_t dirent;
        memset(&dirent, 0, sizeof(dirent));
        name_from_83(&entry, dirent.name);
        dirent.type = (entry.attr & FAT_ATTR_DIRECTORY) ? VFS_DIR : VFS_FILE;
        dirent.size = entry.size;
        if (cb(&dirent, ctx) != 0) {
            break;
        }
    }
    return 0;
}

uint32_t fat16_free_bytes(void)
{
    if (!fs.mounted) {
        return 0;
    }
    uint32_t free_clusters = 0;
    for (uint32_t c = 2; c < fs.max_cluster; ++c) {
        uint16_t value;
        if (fat_read(c, &value) != 0) {
            break;
        }
        if (value == FAT_FREE) {
            free_clusters++;
        }
    }
    return free_clusters * cluster_size();
}

uint32_t fat16_total_bytes(void)
{
    if (!fs.mounted) {
        return 0;
    }
    return (fs.max_cluster - 2) * cluster_size();
}

/* -------------------------------------------------------------------------
 * VFS mount-table registration
 * ---------------------------------------------------------------------- */

static const vfs_ops_t fat16_vfs_ops_table = {
    .stat = fat16_stat,
    .read_file = fat16_read_file,
    .write_file = fat16_write_file,
    .create = fat16_create,
    .unlink = fat16_unlink,
    .rename = fat16_rename,
    .readdir = fat16_readdir,
    /* FAT16 has no on-disk permission/ownership storage — its stat() only
     * synthesizes uid=gid=0 and a fixed mode, so there is nothing for
     * chmod/chown to persist yet. */
    .chmod = NULL,
    .chown = NULL,
};

const vfs_ops_t *fat16_vfs_ops(void)
{
    return &fat16_vfs_ops_table;
}
