#include "file.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

typedef struct copy_ctx {
    uint8_t  *dst;
    uint32_t  remaining;
    uint32_t  block_size;
} copy_ctx_t;

static int copy_block(uint32_t blk_no, void *raw_ctx)
{
    copy_ctx_t *cctx = (copy_ctx_t *)raw_ctx;

    uint32_t chunk = cctx->remaining < cctx->block_size
                     ? cctx->remaining : cctx->block_size;

    if (blk_no == 0) {
        /* Sparse hole — kcalloc already zeroed the output buffer. */
        cctx->dst       += chunk;
        cctx->remaining -= chunk;
        return 0;
    }

    /* Non-owning pointer into block cache — do NOT kfree. */
    const uint8_t *blk = ext2_read_block(blk_no);
    if (!blk) return -1;

    memcpy(cctx->dst, blk, chunk);
    cctx->dst       += chunk;
    cctx->remaining -= chunk;
    return 0;
}

int ext2_read_file_ino(uint32_t ino, uint8_t **out_data, size_t *out_size)
{
    ext2_fs_t    *fs = ext2_get_fs();
    ext2_inode_t  inode;

    if (ext2_read_inode(ino, &inode) != 0) return -1;

    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
        printf("[ext2] file: inode %u is not a regular file (mode=0x%x)\n",
               (unsigned)ino, (unsigned)inode.i_mode);
        return -1;
    }

    uint32_t size = inode.i_size;

    uint8_t *buf = kcalloc(1, size + 1);
    if (!buf) {
        printf("[ext2] file: kcalloc failed for %u bytes\n", (unsigned)size);
        return -1;
    }

    if (size > 0) {
        copy_ctx_t cctx = {
            .dst        = buf,
            .remaining  = size,
            .block_size = fs->block_size,
        };
        if (ext2_iter_blocks(&inode, size, copy_block, &cctx) < 0) {
            kfree(buf);
            return -1;
        }
    }

    *out_data = buf;
    *out_size = size;
    return 0;
}
