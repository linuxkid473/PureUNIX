#include "mount.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "super.h"
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/time.h>

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

    /* Every on-disk inode type is decoded: the coarse VFS type only
     * distinguishes directory/symlink/"everything else" (regular files,
     * device nodes, FIFOs, sockets all read back as VFS_FILE at this
     * level), but the real type is always available via S_ISxxx(st_mode)
     * below. Size always comes straight from the inode — directories are
     * not special-cased to zero. */
    uint16_t type_bits = inode.i_mode & EXT2_S_IFMT;
    if (type_bits == EXT2_S_IFDIR) {
        st->type = VFS_DIR;
    } else if (type_bits == EXT2_S_IFLNK) {
        st->type = VFS_SYMLINK;
    } else {
        st->type = VFS_FILE;
    }
    st->size = inode.i_size;

    /* Expose UNIX permission bits (lower 12 bits of i_mode) as the mode
       field; the VFS treats it as opaque. */
    st->mode = (uint16_t)(inode.i_mode & 0x0FFF);

    /* Full Unix metadata, taken directly from the on-disk inode — EXT2's
     * i_mode already uses the standard S_IF-type-plus-rwx encoding, so it
     * can be copied into st_mode with no translation. No fields synthesized. */
    st->st_mode = inode.i_mode;
    st->st_uid = inode.i_uid;
    st->st_gid = inode.i_gid;
    st->st_nlink = inode.i_links_count;
    st->st_ino = ino;
    st->st_atime = inode.i_atime;
    st->st_mtime = inode.i_mtime;
    st->st_ctime = inode.i_ctime;
    st->st_blocks = inode.i_blocks; /* i_blocks already counts 512-byte sectors */
    st->st_blksize = ext2_get_fs()->block_size;
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

    /* Only regular files have i_block entries that are real data-block
     * pointers. Symlinks (fast symlinks store the target inline in
     * i_block), device nodes, FIFOs, and sockets are refused here rather
     * than iterated as if their inode held block pointers. */
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    if (ext2_read_file_ino(ino, out_data, out_size) != 0) return -1;

    /* Unix semantics: reading a file's data updates its atime. */
    inode.i_atime = time_now();
    ext2_write_inode(ino, &inode);
    return 0;
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

/* -------------------------------------------------------------------------
 * chmod/chown — mutate the on-disk inode's i_mode/i_uid/i_gid directly.
 * Permission checks (owner-or-root for chmod, root-only for chown) already
 * happened in fs/vfs.c's vfs_chmod()/vfs_chown() before either of these is
 * called, so these only need to touch the inode.
 * ---------------------------------------------------------------------- */

int ext2_chmod(const char *path, mode_t mode)
{
    if (!ext2_get_fs()->mounted || !path) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;

    /* Keep the file-type bits (EXT2_S_IFMT) exactly as they are — mode only
     * ever carries permission bits (POSIX chmod(2) ignores any type bits a
     * caller might pass), same 12-bit window ext2_stat() exposes as st->mode. */
    inode.i_mode = (uint16_t)((inode.i_mode & EXT2_S_IFMT) | (mode & 0x0FFF));
    inode.i_ctime = time_now();
    if (ext2_write_inode(ino, &inode) != 0) return -1;
    return 0;
}

int ext2_chown(const char *path, uid_t uid, gid_t gid)
{
    if (!ext2_get_fs()->mounted || !path) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;

    if (uid != (uid_t)-1) {
        inode.i_uid = (uint16_t)uid;
    }
    if (gid != (gid_t)-1) {
        inode.i_gid = (uint16_t)gid;
    }
    inode.i_ctime = time_now();
    if (ext2_write_inode(ino, &inode) != 0) return -1;
    return 0;
}

int ext2_utime(const char *path, uint32_t atime, uint32_t mtime)
{
    if (!ext2_get_fs()->mounted || !path) return -1;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;

    if (atime != 0xFFFFFFFFu) {
        inode.i_atime = atime;
    }
    if (mtime != 0xFFFFFFFFu) {
        inode.i_mtime = mtime;
    }
    inode.i_ctime = time_now();
    if (ext2_write_inode(ino, &inode) != 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * VFS mount-table registration (Stage 4: EXT2 is now writable — see
 * fs/ext2/write.c for create/unlink/rename/link/symlink/readlink/
 * write_file). chmod/chown/utime are real now too — see above.
 * ---------------------------------------------------------------------- */

static const vfs_ops_t ext2_vfs_ops_table = {
    .stat = ext2_stat,
    .read_file = ext2_read_file,
    .write_file = ext2_write_file,
    .create = ext2_create,
    .unlink = ext2_unlink,
    .rename = ext2_rename,
    .readdir = ext2_readdir,
    .chmod = ext2_chmod,
    .chown = ext2_chown,
    .utime = ext2_utime,
    .readlink = ext2_readlink,
    .link = ext2_link,
    .symlink = ext2_symlink,
};

const vfs_ops_t *ext2_vfs_ops(void)
{
    return &ext2_vfs_ops_table;
}
