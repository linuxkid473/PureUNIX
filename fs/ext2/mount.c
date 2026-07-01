#include "mount.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "super.h"
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

/* -------------------------------------------------------------------------
 * Public mount / is-mounted
 * ---------------------------------------------------------------------- */

int ext2_mount(disk_device_t *disk)
{
    if (ext2_get_fs()->mounted) {
        ext2_super_free();
    }
    return ext2_super_read(disk);
}

bool ext2_is_mounted(void)
{
    return ext2_get_fs()->mounted;
}

/* -------------------------------------------------------------------------
 * stat
 * ---------------------------------------------------------------------- */

int ext2_stat(const char *path, vfs_stat_t *st)
{
    if (!ext2_get_fs()->mounted || !path || !st) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;

    uint16_t type_bits = inode.i_mode & EXT2_S_IFMT;
    if (type_bits == EXT2_S_IFDIR) {
        st->type = VFS_DIR;
        st->size = 0;
    } else if (type_bits == EXT2_S_IFREG) {
        st->type = VFS_FILE;
        st->size = inode.i_size;
    } else {
        /* Symlinks, device nodes, etc. — not supported. */
        return -1;
    }

    /* Expose UNIX permission bits (lower 12 bits of i_mode) as the mode
       field; the VFS treats it as opaque. */
    st->mode = (uint16_t)(inode.i_mode & 0x0FFF);
    return 0;
}

/* -------------------------------------------------------------------------
 * read_file — load entire file into a kmalloc'd buffer
 * ---------------------------------------------------------------------- */

int ext2_read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    if (!ext2_get_fs()->mounted || !path) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    return ext2_read_file_ino(ino, out_data, out_size);
}

/* -------------------------------------------------------------------------
 * readdir — enumerate directory entries
 * ---------------------------------------------------------------------- */

int ext2_readdir(const char *path, vfs_readdir_cb_t cb, void *ctx)
{
    if (!ext2_get_fs()->mounted || !path || !cb) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    return ext2_readdir_ino(ino, cb, ctx);
}
