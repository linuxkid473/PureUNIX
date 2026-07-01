/*
 * fs/ext2/write.c — EXT2 write path (Stage 4).
 *
 * Everything here operates purely in terms of inodes and directory entries;
 * none of it knows or cares about the VFS mount table, permissions, or
 * pathname resolution across mounts — that all lives in fs/vfs.c, which
 * hands these functions already-mount-relative sub-paths (e.g. "/etc/foo",
 * never "/mnt/etc/foo"). Every function here returns 0 or a negative errno.
 */
#include "alloc.h"
#include "block.h"
#include "dir.h"
#include "ext2.h"
#include "file.h"
#include "inode.h"
#include "super.h"
#include <pureunix/config.h>
#include <pureunix/errno.h>
#include <pureunix/memory.h>
#include <pureunix/stat.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/time.h>
#include <pureunix/vfs.h>

/* Matches the block-size bound enforced by ext2_super_read() (1024..4096). */
#define EXT2_WRITE_MAX_BLOCK 4096

/* Default permissions for newly created files/directories/symlinks — this
 * driver has no mode/uid/gid parameters on creat()/mkdir() (Stage 4 does
 * not add them; see docs/api/vfs.md), so every new node is owned by the
 * creating task's credentials with a conventional default mode. */
#define EXT2_DEFAULT_FILE_MODE (EXT2_S_IFREG | 0644)
#define EXT2_DEFAULT_DIR_MODE  (EXT2_S_IFDIR | 0755)
#define EXT2_DEFAULT_LINK_MODE (EXT2_S_IFLNK | 0777)

static int split_parent_leaf(const char *path, char *parent, size_t parent_size,
                             char *leaf, size_t leaf_size)
{
    if (!path || path[0] != '/' || strcmp(path, "/") == 0) return -1;

    const char *slash = strrchr(path, '/');
    size_t leaf_len = strlen(slash + 1);
    if (leaf_len == 0 || leaf_len >= leaf_size) return -1;
    strcpy(leaf, slash + 1);

    if (slash == path) {
        strcpy(parent, "/");
    } else {
        size_t plen = (size_t)(slash - path);
        if (plen >= parent_size) return -1;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    return 0;
}

/* Write raw bytes across as many newly-allocated blocks as needed, wiring
 * each one into inode via ext2_inode_add_block(). Does not touch
 * inode->i_size (callers set that themselves once all blocks land). Frees
 * whatever it already allocated on failure. */
static int write_data_blocks(ext2_inode_t *inode, const uint8_t *data, size_t size)
{
    ext2_fs_t *fs = ext2_get_fs();
    uint32_t remaining = (uint32_t)size;
    uint32_t pos = 0;
    uint32_t idx = 0;

    while (remaining > 0) {
        uint32_t blk;
        if (ext2_alloc_block(&blk) != 0) return -ENOSPC;

        uint8_t buf[EXT2_WRITE_MAX_BLOCK];
        uint32_t chunk = remaining < fs->block_size ? remaining : fs->block_size;
        memcpy(buf, data + pos, chunk);
        if (chunk < fs->block_size) memset(buf + chunk, 0, fs->block_size - chunk);
        if (ext2_write_block(blk, buf) != 0) return -EIO;
        if (ext2_inode_add_block(inode, idx, blk) != 0) return -ENOSPC;

        pos += chunk;
        remaining -= chunk;
        idx++;
    }
    return 0;
}

int ext2_create(const char *path, bool directory)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    char parent[PUREUNIX_MAX_PATH], leaf[PUREUNIX_MAX_NAME];
    if (split_parent_leaf(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0) return -ENOENT;

    uint32_t parent_ino;
    if (ext2_path_to_inode(parent, &parent_ino) != 0) return -ENOENT;

    uint32_t existing;
    if (ext2_dir_lookup(parent_ino, leaf, &existing) == 0) return -EEXIST;

    uint32_t new_ino;
    if (ext2_alloc_inode(&new_ino, directory) != 0) return -ENOSPC;

    uint32_t now = time_now();
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = directory ? EXT2_DEFAULT_DIR_MODE : EXT2_DEFAULT_FILE_MODE;
    inode.i_uid = current_uid();
    inode.i_gid = current_gid();
    inode.i_atime = inode.i_ctime = inode.i_mtime = now;
    inode.i_links_count = directory ? 2 : 1;

    if (directory) {
        ext2_fs_t *fs = ext2_get_fs();
        uint32_t blk;
        if (ext2_alloc_block(&blk) != 0) {
            ext2_free_inode(new_ino, true);
            return -ENOSPC;
        }
        uint8_t buf[EXT2_WRITE_MAX_BLOCK];
        memset(buf, 0, fs->block_size);

        ext2_dirent_t *dot = (ext2_dirent_t *)buf;
        dot->inode = new_ino;
        dot->rec_len = 12; /* align4(8+1) */
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        buf[sizeof(ext2_dirent_t)] = '.';

        ext2_dirent_t *dotdot = (ext2_dirent_t *)(buf + 12);
        dotdot->inode = parent_ino;
        dotdot->rec_len = (uint16_t)(fs->block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        buf[12 + sizeof(ext2_dirent_t)] = '.';
        buf[12 + sizeof(ext2_dirent_t) + 1] = '.';

        if (ext2_write_block(blk, buf) != 0) {
            ext2_free_block(blk);
            ext2_free_inode(new_ino, true);
            return -EIO;
        }
        inode.i_block[0] = blk;
        inode.i_blocks = fs->sectors_per_block;
        inode.i_size = fs->block_size;
    }

    if (ext2_write_inode(new_ino, &inode) != 0) {
        if (directory) ext2_inode_free_all_blocks(&inode);
        ext2_free_inode(new_ino, directory);
        return -EIO;
    }

    uint8_t ft = directory ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (ext2_dir_insert(parent_ino, leaf, new_ino, ft) != 0) {
        if (directory) ext2_inode_free_all_blocks(&inode);
        ext2_free_inode(new_ino, directory);
        return -ENOSPC;
    }

    if (directory) {
        ext2_inode_t parent_inode;
        if (ext2_read_inode(parent_ino, &parent_inode) == 0) {
            parent_inode.i_links_count++;
            parent_inode.i_mtime = parent_inode.i_ctime = now;
            ext2_write_inode(parent_ino, &parent_inode);
        }
    }
    return 0;
}

int ext2_unlink(const char *path, bool directory)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    char parent[PUREUNIX_MAX_PATH], leaf[PUREUNIX_MAX_NAME];
    if (split_parent_leaf(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0) return -ENOENT;

    uint32_t parent_ino;
    if (ext2_path_to_inode(parent, &parent_ino) != 0) return -ENOENT;

    uint32_t ino;
    if (ext2_dir_lookup(parent_ino, leaf, &ino) != 0) return -ENOENT;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -EIO;
    bool is_dir = (inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if (directory && !is_dir) return -ENOTDIR;
    if (!directory && is_dir) return -EISDIR;

    if (directory && !ext2_dir_is_empty(ino)) return -ENOTEMPTY;

    if (ext2_dir_remove(parent_ino, leaf) != 0) return -EIO;

    if (directory) {
        ext2_inode_free_all_blocks(&inode);
        ext2_free_inode(ino, true);

        ext2_inode_t parent_inode;
        if (ext2_read_inode(parent_ino, &parent_inode) == 0) {
            if (parent_inode.i_links_count > 0) parent_inode.i_links_count--;
            parent_inode.i_mtime = parent_inode.i_ctime = time_now();
            ext2_write_inode(parent_ino, &parent_inode);
        }
    } else {
        if (inode.i_links_count > 0) inode.i_links_count--;
        if (inode.i_links_count == 0) {
            ext2_inode_free_all_blocks(&inode);
            ext2_free_inode(ino, false);
        } else {
            inode.i_ctime = time_now();
            ext2_write_inode(ino, &inode);
        }
    }
    return 0;
}

int ext2_rename(const char *old_path, const char *new_path)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    char old_parent[PUREUNIX_MAX_PATH], old_leaf[PUREUNIX_MAX_NAME];
    char new_parent[PUREUNIX_MAX_PATH], new_leaf[PUREUNIX_MAX_NAME];
    if (split_parent_leaf(old_path, old_parent, sizeof(old_parent), old_leaf, sizeof(old_leaf)) != 0) return -ENOENT;
    if (split_parent_leaf(new_path, new_parent, sizeof(new_parent), new_leaf, sizeof(new_leaf)) != 0) return -ENOENT;

    uint32_t old_parent_ino, new_parent_ino;
    if (ext2_path_to_inode(old_parent, &old_parent_ino) != 0) return -ENOENT;
    if (ext2_path_to_inode(new_parent, &new_parent_ino) != 0) return -ENOENT;

    uint32_t old_ino;
    if (ext2_dir_lookup(old_parent_ino, old_leaf, &old_ino) != 0) return -ENOENT;

    ext2_inode_t old_inode;
    if (ext2_read_inode(old_ino, &old_inode) != 0) return -EIO;
    bool old_is_dir = (old_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    uint32_t existing_ino;
    bool dest_exists = ext2_dir_lookup(new_parent_ino, new_leaf, &existing_ino) == 0;

    if (dest_exists && existing_ino == old_ino) {
        return 0; /* renaming onto itself: no-op */
    }

    if (dest_exists) {
        ext2_inode_t existing_inode;
        if (ext2_read_inode(existing_ino, &existing_inode) != 0) return -EIO;
        bool existing_is_dir = (existing_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        if (existing_is_dir != old_is_dir) return existing_is_dir ? -EISDIR : -ENOTDIR;
        if (existing_is_dir && !ext2_dir_is_empty(existing_ino)) return -ENOTEMPTY;

        if (ext2_dir_remove(new_parent_ino, new_leaf) != 0) return -EIO;

        if (existing_is_dir) {
            ext2_inode_free_all_blocks(&existing_inode);
            ext2_free_inode(existing_ino, true);
            ext2_inode_t np;
            if (ext2_read_inode(new_parent_ino, &np) == 0) {
                if (np.i_links_count > 0) np.i_links_count--;
                ext2_write_inode(new_parent_ino, &np);
            }
        } else {
            if (existing_inode.i_links_count > 0) existing_inode.i_links_count--;
            if (existing_inode.i_links_count == 0) {
                ext2_inode_free_all_blocks(&existing_inode);
                ext2_free_inode(existing_ino, false);
            } else {
                ext2_write_inode(existing_ino, &existing_inode);
            }
        }
    }

    uint8_t ft = old_is_dir ? EXT2_FT_DIR :
                 ((old_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE);
    if (ext2_dir_insert(new_parent_ino, new_leaf, old_ino, ft) != 0) return -ENOSPC;
    if (ext2_dir_remove(old_parent_ino, old_leaf) != 0) return -EIO;

    if (old_is_dir && old_parent_ino != new_parent_ino) {
        ext2_dir_set_parent(old_ino, new_parent_ino);

        ext2_inode_t op;
        if (ext2_read_inode(old_parent_ino, &op) == 0) {
            if (op.i_links_count > 0) op.i_links_count--;
            ext2_write_inode(old_parent_ino, &op);
        }
        ext2_inode_t np2;
        if (ext2_read_inode(new_parent_ino, &np2) == 0) {
            np2.i_links_count++;
            ext2_write_inode(new_parent_ino, &np2);
        }
    }
    return 0;
}

int ext2_link(const char *old_path, const char *new_path)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    uint32_t old_ino;
    if (ext2_path_to_inode(old_path, &old_ino) != 0) return -ENOENT;

    ext2_inode_t inode;
    if (ext2_read_inode(old_ino, &inode) != 0) return -EIO;
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EPERM;

    char new_parent[PUREUNIX_MAX_PATH], new_leaf[PUREUNIX_MAX_NAME];
    if (split_parent_leaf(new_path, new_parent, sizeof(new_parent), new_leaf, sizeof(new_leaf)) != 0) return -ENOENT;

    uint32_t new_parent_ino;
    if (ext2_path_to_inode(new_parent, &new_parent_ino) != 0) return -ENOENT;

    uint32_t existing;
    if (ext2_dir_lookup(new_parent_ino, new_leaf, &existing) == 0) return -EEXIST;

    uint8_t ft = (inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE;
    if (ext2_dir_insert(new_parent_ino, new_leaf, old_ino, ft) != 0) return -ENOSPC;

    inode.i_links_count++;
    inode.i_ctime = time_now();
    return ext2_write_inode(old_ino, &inode);
}

int ext2_symlink(const char *target, const char *path)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;
    if (!target || !*target) return -EINVAL;

    size_t tlen = strlen(target);
    if (tlen >= PUREUNIX_MAX_PATH) return -ENAMETOOLONG;

    char parent[PUREUNIX_MAX_PATH], leaf[PUREUNIX_MAX_NAME];
    if (split_parent_leaf(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0) return -ENOENT;

    uint32_t parent_ino;
    if (ext2_path_to_inode(parent, &parent_ino) != 0) return -ENOENT;

    uint32_t existing;
    if (ext2_dir_lookup(parent_ino, leaf, &existing) == 0) return -EEXIST;

    uint32_t new_ino;
    if (ext2_alloc_inode(&new_ino, false) != 0) return -ENOSPC;

    uint32_t now = time_now();
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_DEFAULT_LINK_MODE;
    inode.i_uid = current_uid();
    inode.i_gid = current_gid();
    inode.i_atime = inode.i_ctime = inode.i_mtime = now;
    inode.i_links_count = 1;
    inode.i_size = (uint32_t)tlen;

    if (tlen <= 60) {
        /* Fast symlink: target stored inline in i_block, no data block —
         * exactly tools/mkext2.py's add_symlink() convention. */
        memcpy((uint8_t *)inode.i_block, target, tlen);
    } else {
        int rc = write_data_blocks(&inode, (const uint8_t *)target, tlen);
        if (rc != 0) {
            ext2_inode_free_all_blocks(&inode);
            ext2_free_inode(new_ino, false);
            return rc;
        }
    }

    if (ext2_write_inode(new_ino, &inode) != 0) {
        ext2_inode_free_all_blocks(&inode);
        ext2_free_inode(new_ino, false);
        return -EIO;
    }

    if (ext2_dir_insert(parent_ino, leaf, new_ino, EXT2_FT_SYMLINK) != 0) {
        ext2_inode_free_all_blocks(&inode);
        ext2_free_inode(new_ino, false);
        return -ENOSPC;
    }
    return 0;
}

/* Copies bytes out of an inode's data-block chain (used for long symlink
 * targets that didn't fit inline) — mirrors fs/ext2/file.c's copy_block,
 * duplicated locally since that one is static to file.c and this is the
 * only other place block-chain-to-buffer copying is needed. */
typedef struct link_copy_ctx {
    uint8_t *dst;
    uint32_t remaining;
    uint32_t block_size;
} link_copy_ctx_t;

static int link_copy_block(uint32_t blk_no, void *raw_ctx)
{
    link_copy_ctx_t *cctx = raw_ctx;
    uint32_t chunk = cctx->remaining < cctx->block_size ? cctx->remaining : cctx->block_size;
    if (blk_no != 0) {
        const uint8_t *blk = ext2_read_block(blk_no);
        if (!blk) return -1;
        memcpy(cctx->dst, blk, chunk);
    }
    cctx->dst += chunk;
    cctx->remaining -= chunk;
    return 0;
}

int ext2_readlink(const char *path, char *buf, size_t bufsize)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) return -ENOENT;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -EIO;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return -EINVAL;

    uint32_t len = inode.i_size;
    uint32_t to_copy = len < bufsize ? len : (uint32_t)bufsize;

    if (inode.i_blocks == 0) {
        /* Fast symlink: target lives directly in i_block's bytes. */
        memcpy(buf, (const uint8_t *)inode.i_block, to_copy);
    } else {
        ext2_fs_t *fs = ext2_get_fs();
        link_copy_ctx_t cctx = { .dst = (uint8_t *)buf, .remaining = to_copy, .block_size = fs->block_size };
        if (ext2_iter_blocks(&inode, to_copy, link_copy_block, &cctx) < 0) return -EIO;
    }
    return (int)to_copy;
}

int ext2_write_file(const char *path, const uint8_t *data, size_t size, uint32_t flags)
{
    if (!ext2_get_fs()->mounted) return -ENOENT;

    uint32_t ino;
    if (ext2_path_to_inode(path, &ino) != 0) {
        int rc = ext2_create(path, false);
        if (rc != 0) return rc;
        if (ext2_path_to_inode(path, &ino) != 0) return -ENOENT;
    }

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -EIO;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -EISDIR;

    const uint8_t *final_data = data;
    size_t final_size = size;
    uint8_t *combined = NULL;

    if (flags & VFS_O_APPEND) {
        uint8_t *old = NULL;
        size_t old_size = 0;
        if (ext2_read_file_ino(ino, &old, &old_size) == 0) {
            combined = kmalloc(old_size + size);
            if (!combined) {
                kfree(old);
                return -ENOSPC;
            }
            memcpy(combined, old, old_size);
            memcpy(combined + old_size, data, size);
            kfree(old);
            final_data = combined;
            final_size = old_size + size;
        }
    }

    ext2_inode_free_all_blocks(&inode);
    int rc = write_data_blocks(&inode, final_data, final_size);
    kfree(combined);
    if (rc != 0) {
        /* write_data_blocks() can fail partway (e.g. -ENOSPC), after having
         * already allocated some new blocks into inode.i_block[] (in
         * memory only) — and the old blocks were already freed on disk by
         * ext2_inode_free_all_blocks() above. Returning here without
         * persisting anything would leave the on-disk inode still
         * pointing at those now-freed old blocks, which the allocator is
         * free to hand to an unrelated file next: a later read of this
         * file would then silently return someone else's data. Free
         * whatever the failed attempt did manage to allocate and persist
         * the inode as the empty file it now actually is, so nothing is
         * left dangling or leaked. */
        ext2_inode_free_all_blocks(&inode);
        ext2_write_inode(ino, &inode);
        return rc;
    }

    inode.i_size = (uint32_t)final_size;
    inode.i_mtime = inode.i_ctime = time_now();
    return ext2_write_inode(ino, &inode);
}
