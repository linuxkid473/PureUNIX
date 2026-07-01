/*
 * fs/ext2/ext2.h — EXT2 on-disk data structures and internal driver types.
 *
 * All structures are packed to match the exact on-disk byte layout.
 * Fields are named to match the Linux EXT2 specification (the authoritative
 * reference is Documentation/filesystems/ext2.txt in the Linux source tree).
 */
#ifndef EXT2_INT_H
#define EXT2_INT_H

#include <pureunix/disk.h>
#include <pureunix/types.h>

/* -------------------------------------------------------------------------
 * EXT2 constants
 * ---------------------------------------------------------------------- */

#define EXT2_MAGIC          0xEF53  /* superblock magic number              */
#define EXT2_ROOT_INODE     2       /* inode number of the root directory   */
#define EXT2_BAD_INO        1       /* inode number of the bad-blocks list  */
#define EXT2_MIN_INODE_SIZE 128     /* inode size in revision 0             */

/* s_rev_level values */
#define EXT2_GOOD_OLD_REV   0       /* original EXT2 revision               */
#define EXT2_DYNAMIC_REV    1       /* revision with variable inode size    */

/* Incompatible feature flags — we must refuse to mount if any are set. */
#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE    0x0002  /* dir entries have type field */
#define EXT2_FEATURE_INCOMPAT_RECOVER     0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG     0x0010

/* We accept FILETYPE (directory entry type field) but nothing else. */
#define EXT2_SUPPORTED_INCOMPAT  EXT2_FEATURE_INCOMPAT_FILETYPE

/* Inode i_mode type bits */
#define EXT2_S_IFREG  0x8000   /* regular file                             */
#define EXT2_S_IFDIR  0x4000   /* directory                                */
#define EXT2_S_IFLNK  0xA000   /* symbolic link (not supported)            */
#define EXT2_S_IFMT   0xF000   /* bitmask for inode type field             */

/* Directory entry file_type values (only valid when FILETYPE feature set) */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_SYMLINK  7

/* -------------------------------------------------------------------------
 * Superblock — always located at byte offset 1024 from the FS start.
 * For 512-byte sectors that means LBA 2 (sector 2) of the device.
 * Total size: 1024 bytes.
 * ---------------------------------------------------------------------- */
typedef struct ext2_superblock {
    /* base fields (present in all revisions) */
    uint32_t s_inodes_count;        /* total number of inodes               */
    uint32_t s_blocks_count;        /* total number of blocks               */
    uint32_t s_r_blocks_count;      /* reserved blocks (for superuser)      */
    uint32_t s_free_blocks_count;   /* count of free blocks                 */
    uint32_t s_free_inodes_count;   /* count of free inodes                 */
    uint32_t s_first_data_block;    /* block number of first data block     *
                                     * 1 for 1 KB blocks, 0 for larger     */
    uint32_t s_log_block_size;      /* block_size = 1024 << s_log_block_size*/
    uint32_t s_log_frag_size;       /* fragment size (same as block size)   */
    uint32_t s_blocks_per_group;    /* number of blocks per block group     */
    uint32_t s_frags_per_group;     /* number of fragments per group        */
    uint32_t s_inodes_per_group;    /* number of inodes per block group     */
    uint32_t s_mtime;               /* time of last mount (UNIX epoch)      */
    uint32_t s_wtime;               /* time of last write (UNIX epoch)      */
    uint16_t s_mnt_count;           /* number of mounts since last fsck     */
    uint16_t s_max_mnt_count;       /* max mounts before fsck is required   */
    uint16_t s_magic;               /* EXT2 magic: 0xEF53                  */
    uint16_t s_state;               /* filesystem state (1=clean, 2=errors) */
    uint16_t s_errors;              /* error handling: 1=continue, 2=ro    */
    uint16_t s_minor_rev_level;     /* minor revision level                 */
    uint32_t s_lastcheck;           /* time of last fsck                    */
    uint32_t s_checkinterval;       /* max interval between fscks           */
    uint32_t s_creator_os;          /* OS that created the filesystem       */
    uint32_t s_rev_level;           /* revision level (0=old, 1=dynamic)   */
    uint16_t s_def_resuid;          /* default UID for reserved blocks      */
    uint16_t s_def_resgid;          /* default GID for reserved blocks      */

    /* extended fields — only valid when s_rev_level >= EXT2_DYNAMIC_REV */
    uint32_t s_first_ino;           /* first non-reserved inode number      */
    uint16_t s_inode_size;          /* size of inode structure in bytes     */
    uint16_t s_block_group_nr;      /* block group containing this SB       */
    uint32_t s_feature_compat;      /* compatible feature flags             */
    uint32_t s_feature_incompat;    /* incompatible feature flags           */
    uint32_t s_feature_ro_compat;   /* read-only compatible feature flags   */
    uint8_t  s_uuid[16];            /* 128-bit volume UUID                  */
    char     s_volume_name[16];     /* null-terminated volume name          */
    char     s_last_mounted[64];    /* path where last mounted              */
    uint32_t s_algo_bitmap;         /* compression algorithm used           */

    /* performance hints */
    uint8_t  s_prealloc_blocks;     /* blocks to preallocate for files      */
    uint8_t  s_prealloc_dir_blocks; /* blocks to preallocate for dirs       */
    uint16_t s_padding1;

    /* journaling (ext3+) — not used by us */
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;

    /* hash seed for directory indexing */
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_word_pad;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;

    uint8_t  s_reserved[760];       /* pad to 1024 bytes                   */
} __attribute__((packed)) ext2_superblock_t;

/* -------------------------------------------------------------------------
 * Block Group Descriptor — 32 bytes per entry.
 * The table starts at block (s_first_data_block + 1).
 * ---------------------------------------------------------------------- */
typedef struct ext2_bgdt_entry {
    uint32_t bg_block_bitmap;       /* block number of the block-use bitmap */
    uint32_t bg_inode_bitmap;       /* block number of the inode-use bitmap */
    uint32_t bg_inode_table;        /* starting block of the inode table    */
    uint16_t bg_free_blocks_count;  /* number of free blocks in this group  */
    uint16_t bg_free_inodes_count;  /* number of free inodes in this group  */
    uint16_t bg_used_dirs_count;    /* number of directories in this group  */
    uint16_t bg_pad;                /* padding to align to 4 bytes          */
    uint32_t bg_reserved[3];        /* reserved — must be zero              */
} __attribute__((packed)) ext2_bgdt_entry_t;

/* -------------------------------------------------------------------------
 * Inode — 128 bytes (revision 0) or s_inode_size bytes (revision 1+).
 * ---------------------------------------------------------------------- */
typedef struct ext2_inode {
    uint16_t i_mode;        /* file type + permission bits (see EXT2_S_*)   */
    uint16_t i_uid;         /* lower 16 bits of owner UID                   */
    uint32_t i_size;        /* file size in bytes (lower 32 bits)           */
    uint32_t i_atime;       /* last access time (seconds since epoch)       */
    uint32_t i_ctime;       /* inode change time (seconds since epoch)      */
    uint32_t i_mtime;       /* last data modification time (seconds)        */
    uint32_t i_dtime;       /* deletion time (0 if not deleted)             */
    uint16_t i_gid;         /* lower 16 bits of owner GID                   */
    uint16_t i_links_count; /* number of hard links to this inode           */
    uint32_t i_blocks;      /* count of 512-byte blocks allocated           *
                             * NOTE: this counts disk sectors, NOT FS blocks*/
    uint32_t i_flags;       /* inode flags (e.g. EXT2_SECRM_FL)            */
    uint32_t i_osd1;        /* OS-specific value 1 (ignore)                 */
    uint32_t i_block[15];   /* block pointers:                              *
                             *   [0..11] direct blocks                      *
                             *   [12]    singly-indirect block pointer      *
                             *   [13]    doubly-indirect (NOT IMPLEMENTED)  *
                             *   [14]    triply-indirect (NOT IMPLEMENTED)  */
    uint32_t i_generation;  /* file version for NFS                         */
    uint32_t i_file_acl;    /* extended attribute block (file)              */
    uint32_t i_dir_acl;     /* for regular files: upper 32 bits of i_size  *
                             * for directories: extended attribute block    */
    uint32_t i_faddr;       /* fragment address (obsolete)                  */
    uint8_t  i_osd2[12];    /* OS-specific value 2 (ignore)                 */
} __attribute__((packed)) ext2_inode_t;

/* -------------------------------------------------------------------------
 * Directory entry — variable length, 4-byte aligned.
 * Entries are packed sequentially in a data block.  To advance to the next
 * entry, add rec_len to the current entry's byte offset.
 * The name field immediately follows this header and is NOT null-terminated.
 * ---------------------------------------------------------------------- */
typedef struct ext2_dirent {
    uint32_t inode;         /* inode number (0 = unused/deleted entry)      */
    uint16_t rec_len;       /* total length of this entry in bytes          *
                             * (always a multiple of 4)                     */
    uint8_t  name_len;      /* length of the name in bytes                  */
    uint8_t  file_type;     /* EXT2_FT_* (only valid if FILETYPE feature)  */
    /* char name[] follows immediately — access via EXT2_DIRENT_NAME()     */
} __attribute__((packed)) ext2_dirent_t;

/* Convenience: pointer to the name bytes immediately after the dirent header. */
#define EXT2_DIRENT_NAME(de)  ((const char *)((const uint8_t *)(de) + sizeof(ext2_dirent_t)))

/* -------------------------------------------------------------------------
 * In-memory filesystem state (analogous to fat16_fs_t).
 * ---------------------------------------------------------------------- */
typedef struct ext2_fs {
    disk_device_t      *disk;
    bool                mounted;

    uint32_t            block_size;         /* bytes per block               */
    uint32_t            sectors_per_block;  /* block_size / 512              */
    uint32_t            inodes_per_group;
    uint32_t            blocks_per_group;
    uint32_t            inode_size;         /* bytes per inode structure     */
    uint32_t            first_data_block;   /* s_first_data_block value      */
    uint32_t            num_groups;         /* number of block groups        */
    uint32_t            total_inodes;
    uint32_t            total_blocks;
    uint32_t            rev_level;          /* 0 or 1                        */

    ext2_bgdt_entry_t  *bgdt;              /* kmalloc'd BGDT array          */
} ext2_fs_t;

#endif /* EXT2_INT_H */
