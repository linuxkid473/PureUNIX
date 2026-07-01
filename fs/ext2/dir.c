#include "dir.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include <pureunix/config.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vfs.h>

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

    /* Non-owning pointer into block cache — do NOT kfree. */
    const uint8_t *buf = ext2_read_block(blk_no);
    if (!buf) return -1;

    uint32_t off = 0;
    while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)(buf + off);
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len > 0) {
            const char *n    = EXT2_DIRENT_NAME(de);
            uint8_t     nlen = de->name_len;
            if (!((nlen == 1 && n[0] == '.') ||
                  (nlen == 2 && n[0] == '.' && n[1] == '.'))) {

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
                    dirent.type = VFS_FILE;
                    ext2_inode_t file_inode;
                    dirent.size = (ext2_read_inode(de->inode, &file_inode) == 0)
                                  ? file_inode.i_size : 0;
                }

                if (rctx->cb(&dirent, rctx->user_ctx) != 0) {
                    rctx->stopped = 1;
                    return 1;  /* stop iteration */
                }
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
