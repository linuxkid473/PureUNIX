#include "dir.h"
#include "alloc.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include <pureunix/config.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/time.h>
#include <pureunix/vfs.h>

/* Matches the block-size bound enforced by ext2_super_read() (1024..4096). */
#define EXT2_DIR_MAX_BLOCK 4096

static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

/* Resolve inode's index'th data block number (direct or singly-indirect),
   for directory-modification code that needs random access by block index
   rather than the streaming order ext2_iter_blocks provides. Returns 0 if
   the block doesn't exist (sparse hole or out of range). */
static uint32_t inode_block_at(const ext2_inode_t *inode, uint32_t index)
{
    if (index < 12) return inode->i_block[index];
    if (inode->i_block[12] == 0) return 0;

    ext2_fs_t *fs = ext2_get_fs();
    uint32_t ind_index = index - 12;
    if (ind_index >= fs->block_size / sizeof(uint32_t)) return 0;

    const uint8_t *ro = ext2_read_block(inode->i_block[12]);
    if (!ro) return 0;
    uint32_t val;
    memcpy(&val, ro + ind_index * sizeof(uint32_t), sizeof(uint32_t));
    return val;
}

/* -------------------------------------------------------------------------
 * Directory block search callback context
 * ---------------------------------------------------------------------- */
typedef struct lookup_ctx {
    const char *name;
    size_t      name_len;
    uint32_t    found_ino;
} lookup_ctx_t;

static int lookup_in_block(uint32_t blk_no, void *raw_ctx)
{
    lookup_ctx_t *lctx = (lookup_ctx_t *)raw_ctx;
    ext2_fs_t    *fs   = ext2_get_fs();

    if (blk_no == 0) return 0;   /* sparse hole — directories never have these */

    /* Non-owning pointer into block cache — do NOT kfree. */
    const uint8_t *buf = ext2_read_block(blk_no);
    if (!buf) return -1;

    uint32_t off = 0;
    while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)(buf + off);
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len == (uint8_t)lctx->name_len) {
            if (strncmp(EXT2_DIRENT_NAME(de), lctx->name, lctx->name_len) == 0) {
                lctx->found_ino = de->inode;
                return 1;   /* found — stop iteration */
            }
        }
        off += de->rec_len;
    }

    return 0;   /* not in this block — keep going */
}

int ext2_dir_lookup(uint32_t dir_ino, const char *name, uint32_t *out_ino)
{
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) return -1;

    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        printf("[ext2] dir_lookup: inode %u is not a directory\n",
               (unsigned)dir_ino);
        return -1;
    }

    lookup_ctx_t ctx = { .name = name, .name_len = strlen(name), .found_ino = 0 };

    int r = ext2_iter_blocks(&inode, inode.i_size, lookup_in_block, &ctx);
    if (r == 1 && ctx.found_ino != 0) {
        *out_ino = ctx.found_ino;
        return 0;
    }
    return -1;
}

int ext2_path_to_inode(const char *path, uint32_t *out_ino)
{
    if (!path || path[0] != '/') return -1;

    if (strcmp(path, "/") == 0) {
        *out_ino = EXT2_ROOT_INODE;
        return 0;
    }

    char     tmp[PUREUNIX_MAX_PATH];
    char    *save = NULL;
    uint32_t cur  = EXT2_ROOT_INODE;

    strncpy(tmp, path + 1, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *tok = strtok_r(tmp, "/", &save); tok;
              tok = strtok_r(NULL, "/", &save)) {
        uint32_t next;
        if (ext2_dir_lookup(cur, tok, &next) != 0) return -1;
        cur = next;
    }

    *out_ino = cur;
    return 0;
}

/* -------------------------------------------------------------------------
 * readdir — enumerate all entries in a directory inode
 * ---------------------------------------------------------------------- */

typedef struct readdir_ctx {
    vfs_readdir_cb_t  cb;
    void             *user_ctx;
    int               stopped;
} readdir_ctx_t;

static int readdir_block(uint32_t blk_no, void *raw_ctx)
{
    readdir_ctx_t *rctx = (readdir_ctx_t *)raw_ctx;
    ext2_fs_t     *fs   = ext2_get_fs();

    if (blk_no == 0) return 0;

    /* Copy the block out immediately: unlike a lookup that only compares
     * names, this loop calls ext2_read_inode() per entry (to fill in
     * dirent.size), which itself calls ext2_read_block() on the inode
     * table — a different block competing for the same cache slots. Held
     * across that call, a raw cache pointer here could get silently
     * evicted and start reading someone else's block mid-scan (see the
     * ownership rule in block.h; every other block-modifying loop in this
     * file already copies out for the same reason). */
    const uint8_t *ro = ext2_read_block(blk_no);
    if (!ro) return -1;
    uint8_t buf[EXT2_DIR_MAX_BLOCK];
    memcpy(buf, ro, fs->block_size);

    uint32_t off = 0;
    while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)(buf + off);
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len > 0) {
            const char *n    = EXT2_DIRENT_NAME(de);
            uint8_t     nlen = de->name_len;

            /* "." and ".." are real directory entries on disk (every EXT2
             * directory stores them) and are passed through here just like
             * any other entry; it's the shell's job to hide them by default
             * and show them under ls -a. */
            vfs_dirent_t dirent;
            memset(&dirent, 0, sizeof(dirent));

            uint8_t copy_len = nlen < (PUREUNIX_MAX_NAME - 1)
                               ? nlen : (PUREUNIX_MAX_NAME - 1);
            memcpy(dirent.name, n, copy_len);
            dirent.name[copy_len] = '\0';

            if (de->file_type == EXT2_FT_DIR) {
                dirent.type = VFS_DIR;
                dirent.size = 0;
            } else {
                dirent.type = (de->file_type == EXT2_FT_SYMLINK) ? VFS_SYMLINK : VFS_FILE;
                ext2_inode_t file_inode;
                dirent.size = (ext2_read_inode(de->inode, &file_inode) == 0)
                              ? file_inode.i_size : 0;
            }

            if (rctx->cb(&dirent, rctx->user_ctx) != 0) {
                rctx->stopped = 1;
                return 1;  /* stop iteration */
            }
        }
        off += de->rec_len;
    }

    return 0;
}

int ext2_readdir_ino(uint32_t dir_ino, vfs_readdir_cb_t cb, void *ctx)
{
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    readdir_ctx_t rctx = { .cb = cb, .user_ctx = ctx, .stopped = 0 };
    ext2_iter_blocks(&inode, inode.i_size, readdir_block, &rctx);
    return rctx.stopped ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Directory modification — insert / remove (Stage 4)
 * ---------------------------------------------------------------------- */

int ext2_dir_insert(uint32_t dir_ino, const char *name, uint32_t ino, uint8_t file_type)
{
    ext2_fs_t *fs = ext2_get_fs();
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0) return -1;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    uint32_t needed = align4((uint32_t)sizeof(ext2_dirent_t) + (uint32_t)name_len);

    uint32_t nblocks = dir.i_size / fs->block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk_no = inode_block_at(&dir, b);
        if (blk_no == 0) continue;

        const uint8_t *ro = ext2_read_block(blk_no);
        if (!ro) return -1;
        uint8_t buf[EXT2_DIR_MAX_BLOCK];
        memcpy(buf, ro, fs->block_size);

        uint32_t off = 0;
        bool placed = false;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode == 0) {
                /* Deleted/unused slot: reuse outright if it fits. */
                if (de->rec_len >= needed) {
                    de->inode = ino;
                    de->name_len = (uint8_t)name_len;
                    de->file_type = file_type;
                    memcpy((uint8_t *)de + sizeof(ext2_dirent_t), name, name_len);
                    placed = true;
                    break;
                }
            } else {
                uint32_t actual = align4((uint32_t)sizeof(ext2_dirent_t) + de->name_len);
                uint32_t slack = de->rec_len - actual;
                if (slack >= needed) {
                    uint16_t old_rec_len = de->rec_len;
                    de->rec_len = (uint16_t)actual;

                    ext2_dirent_t *nde = (ext2_dirent_t *)(buf + off + actual);
                    nde->inode = ino;
                    nde->rec_len = (uint16_t)(old_rec_len - actual);
                    nde->name_len = (uint8_t)name_len;
                    nde->file_type = file_type;
                    memcpy((uint8_t *)nde + sizeof(ext2_dirent_t), name, name_len);
                    placed = true;
                    break;
                }
            }
            off += de->rec_len;
        }

        if (placed) {
            if (ext2_write_block(blk_no, buf) != 0) return -1;
            dir.i_mtime = dir.i_ctime = time_now();
            return ext2_write_inode(dir_ino, &dir);
        }
    }

    /* No existing block had room: grow the directory by one block. */
    uint32_t new_blk;
    if (ext2_alloc_block(&new_blk) != 0) return -1;
    if (ext2_inode_add_block(&dir, nblocks, new_blk) != 0) return -1;

    uint8_t buf[EXT2_DIR_MAX_BLOCK];
    memset(buf, 0, fs->block_size);
    ext2_dirent_t *de = (ext2_dirent_t *)buf;
    de->inode = ino;
    de->rec_len = (uint16_t)fs->block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(buf + sizeof(ext2_dirent_t), name, name_len);
    if (ext2_write_block(new_blk, buf) != 0) return -1;

    dir.i_size += fs->block_size;
    dir.i_mtime = dir.i_ctime = time_now();
    return ext2_write_inode(dir_ino, &dir);
}

int ext2_dir_remove(uint32_t dir_ino, const char *name)
{
    ext2_fs_t *fs = ext2_get_fs();
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0) return -1;

    size_t name_len = strlen(name);
    uint32_t nblocks = dir.i_size / fs->block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk_no = inode_block_at(&dir, b);
        if (blk_no == 0) continue;

        const uint8_t *ro = ext2_read_block(blk_no);
        if (!ro) return -1;
        uint8_t buf[EXT2_DIR_MAX_BLOCK];
        memcpy(buf, ro, fs->block_size);

        uint32_t off = 0;
        uint32_t prev_off = (uint32_t)-1;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == (uint8_t)name_len &&
                strncmp(EXT2_DIRENT_NAME(de), name, name_len) == 0) {
                if (prev_off != (uint32_t)-1) {
                    ext2_dirent_t *prev = (ext2_dirent_t *)(buf + prev_off);
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    de->inode = 0; /* first in block: leave as a reusable free slot */
                }
                if (ext2_write_block(blk_no, buf) != 0) return -1;
                dir.i_mtime = dir.i_ctime = time_now();
                return ext2_write_inode(dir_ino, &dir);
            }
            prev_off = off;
            off += de->rec_len;
        }
    }
    return -1; /* not found */
}

bool ext2_dir_is_empty(uint32_t dir_ino)
{
    ext2_fs_t *fs = ext2_get_fs();
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0) return false;

    uint32_t nblocks = dir.i_size / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk_no = inode_block_at(&dir, b);
        if (blk_no == 0) continue;
        const uint8_t *buf = ext2_read_block(blk_no);
        if (!buf) return false;

        uint32_t off = 0;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            const ext2_dirent_t *de = (const ext2_dirent_t *)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0) {
                const char *n = EXT2_DIRENT_NAME(de);
                bool dot = de->name_len == 1 && n[0] == '.';
                bool dotdot = de->name_len == 2 && n[0] == '.' && n[1] == '.';
                if (!dot && !dotdot) return false;
            }
            off += de->rec_len;
        }
    }
    return true;
}

int ext2_dir_set_parent(uint32_t dir_ino, uint32_t new_parent_ino)
{
    ext2_fs_t *fs = ext2_get_fs();
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0) return -1;

    uint32_t nblocks = dir.i_size / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk_no = inode_block_at(&dir, b);
        if (blk_no == 0) continue;
        const uint8_t *ro = ext2_read_block(blk_no);
        if (!ro) return -1;
        uint8_t buf[EXT2_DIR_MAX_BLOCK];
        memcpy(buf, ro, fs->block_size);

        uint32_t off = 0;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == 2) {
                const char *n = EXT2_DIRENT_NAME(de);
                if (n[0] == '.' && n[1] == '.') {
                    de->inode = new_parent_ino;
                    return ext2_write_block(blk_no, buf);
                }
            }
            off += de->rec_len;
        }
    }
    return -1;
}
