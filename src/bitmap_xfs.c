/*
 * bitmap_xfs.c — read XFS block allocation bitmap
 *
 * XFS uses B+tree per Allocation Group (AG) for free space tracking,
 * not a simple bitmap like ext4.  We use two approaches:
 *
 *   Approach 1 (preferred): Use FICLONE/FIBMAP ioctls or XFS_IOC_GETBMAPX
 *                           on a mounted filesystem — requires mount.
 *   Approach 2 (offline):   Parse AG headers and free space B+tree from raw disk.
 *
 * For emergency response (disk likely unmounted or from Live USB), we implement
 * Approach 2: direct parsing of XFS on-disk structures.
 */
#include "common.h"
#include <arpa/inet.h>   /* ntohl, be32/be64 conversion */
#include <byteswap.h>

/* XFS is big-endian on disk */
static inline uint16_t xfs_be16(uint16_t v) { return ntohs(v); }
static inline uint32_t xfs_be32(uint32_t v) { return ntohl(v); }
static inline uint64_t xfs_be64(uint64_t v) { return __bswap_64(v); }

/* ========================================================================= */
/*  XFS on-disk structures (minimal subset)                                   */
/* ========================================================================= */

#pragma pack(push, 1)

/* Superblock (at offset 0 of the partition) */
typedef struct {
    uint32_t sb_magicnum;       /* "XFSB" = 0x58465342 */
    uint32_t sb_blocksize;
    uint64_t sb_dblocks;        /* total data blocks in filesystem */
    uint64_t sb_rblocks;
    uint64_t sb_rextents;
    uint8_t  sb_uuid[16];
    uint64_t sb_logstart;
    uint64_t sb_rootino;
    uint64_t sb_rbmino;
    uint64_t sb_rsumino;
    uint32_t sb_rextsize;
    uint32_t sb_agblocks;       /* blocks per AG */
    uint32_t sb_agcount;        /* number of AGs */
    uint32_t sb_rbmblocks;
    uint32_t sb_logblocks;
    uint16_t sb_versionnum;
    uint16_t sb_sectsize;
    uint16_t sb_inodesize;
    uint16_t sb_inopblock;
    char     sb_fname[12];      /* filesystem name */
    uint8_t  sb_blocklog;
    uint8_t  sb_sectlog;
    uint8_t  sb_inodelog;
    uint8_t  sb_inopblog;
    uint8_t  sb_agblklog;
    uint8_t  _pad1[7];
    uint64_t sb_icount;
    uint64_t sb_ifree;
    uint64_t sb_fdblocks;       /* free data blocks */
} xfs_sb_t;

/* AG Free Space Block (AGF) — at sector 1 of each AG */
typedef struct {
    uint32_t agf_magicnum;      /* "XAGF" = 0x58414746 */
    uint32_t agf_versionnum;
    uint32_t agf_seqno;         /* AG number */
    uint32_t agf_length;        /* AG size in blocks */
    uint32_t agf_roots[2];      /* root blocks: [0]=bnobt, [1]=cntbt */
    uint32_t agf_spare0;
    uint32_t agf_levels[2];     /* btree levels */
    uint32_t agf_spare1;
    uint32_t agf_flfirst;
    uint32_t agf_fllast;
    uint32_t agf_flcount;
    uint32_t agf_freeblks;
    uint32_t agf_longest;
} xfs_agf_t;

/* B+tree block header (short form, used in AG btrees) */
typedef struct {
    uint32_t bb_magic;
    uint16_t bb_level;
    uint16_t bb_numrecs;
    uint32_t bb_leftsib;
    uint32_t bb_rightsib;
} xfs_btree_sblock_t;

/* Free space by-block B+tree leaf record */
typedef struct {
    uint32_t ar_startblock;
    uint32_t ar_blockcount;
} xfs_alloc_rec_t;

#pragma pack(pop)

#define XFS_AGF_MAGIC   0x58414746  /* "XAGF" */
#define XFS_ABTB_MAGIC  0x41425442  /* "ABTB" — free space by-block btree */
#define XFS_ABTB_CRC_MAGIC 0x41423342  /* "AB3B" — v5 with CRC */

/* ========================================================================= */
/*  Recursive B+tree traversal to collect free extents                        */
/* ========================================================================= */

static int xfs_walk_bnobt(int fd, uint64_t ag_offset, uint32_t block_size,
                          uint32_t blkno, uint16_t level,
                          uint8_t *bitmap, uint64_t ag_block_base,
                          uint64_t total_blocks)
{
    uint8_t *buf = (uint8_t *)malloc(block_size);
    if (!buf) return -1;

    uint64_t offset = ag_offset + (uint64_t)blkno * block_size;
    if (pread(fd, buf, block_size, offset) != (ssize_t)block_size) {
        free(buf);
        return -1;
    }

    xfs_btree_sblock_t *hdr = (xfs_btree_sblock_t *)buf;
    uint16_t numrecs = xfs_be16(hdr->bb_numrecs);
    uint16_t blevel  = xfs_be16(hdr->bb_level);

    /* Determine header size (v4 vs v5 with CRC) */
    uint32_t magic = xfs_be32(hdr->bb_magic);
    uint32_t hdr_size;
    if (magic == XFS_ABTB_CRC_MAGIC)
        hdr_size = 56;  /* v5 short btree block header */
    else
        hdr_size = 16;  /* v4 short btree block header */

    if (blevel == 0) {
        /* Leaf node: records are free extents — mark them as FREE (clear bits) */
        /* Actually, our bitmap starts as all-1 (used), so we clear free ranges */
        xfs_alloc_rec_t *recs = (xfs_alloc_rec_t *)(buf + hdr_size);
        for (uint16_t i = 0; i < numrecs; i++) {
            uint32_t start = xfs_be32(recs[i].ar_startblock);
            uint32_t count = xfs_be32(recs[i].ar_blockcount);
            /* Clear bits for free blocks */
            for (uint32_t b = 0; b < count; b++) {
                uint64_t abs_block = ag_block_base + start + b;
                if (abs_block < total_blocks) {
                    bitmap[abs_block / 8] &= ~(1u << (abs_block % 8));
                }
            }
        }
    } else {
        /* Internal node: keys + pointers. Pointers start after keys. */
        /* For short-form btree: ptrs are uint32_t AG block numbers */
        /* Keys are at hdr_size, each key is 8 bytes (startblock + blockcount) */
        /* Pointers are at hdr_size + numrecs * 8 */
        uint32_t *ptrs = (uint32_t *)(buf + hdr_size + numrecs * 2 * sizeof(uint32_t));
        for (uint16_t i = 0; i < numrecs; i++) {
            uint32_t child_blk = xfs_be32(ptrs[i]);
            if (xfs_walk_bnobt(fd, ag_offset, block_size, child_blk, blevel - 1,
                               bitmap, ag_block_base, total_blocks) < 0) {
                free(buf);
                return -1;
            }
        }
    }

    free(buf);
    return 0;
}

/* ========================================================================= */
/*  Public interface                                                          */
/* ========================================================================= */

int xfs_read_bitmap(int fd, uint64_t part_offset, uint64_t part_size,
                    block_bitmap_t *bm)
{
    memset(bm, 0, sizeof(*bm));

    /* 1. Read superblock */
    xfs_sb_t sb;
    if (pread(fd, &sb, sizeof(sb), part_offset) != sizeof(sb)) {
        fprintf(stderr, "xfs: failed to read superblock\n");
        return -1;
    }
    if (xfs_be32(sb.sb_magicnum) != 0x58465342) {
        fprintf(stderr, "xfs: bad magic\n");
        return -1;
    }

    uint32_t block_size = xfs_be32(sb.sb_blocksize);
    uint64_t total_blocks = xfs_be64(sb.sb_dblocks);
    uint32_t ag_blocks = xfs_be32(sb.sb_agblocks);
    uint32_t ag_count  = xfs_be32(sb.sb_agcount);

    /* 2. Allocate bitmap — start with all bits SET (all used) */
    bm->total_blocks = total_blocks;
    bm->block_size = block_size;
    bm->bitmap_byte_len = (total_blocks + 7) / 8;
    bm->bitmap = (uint8_t *)malloc(bm->bitmap_byte_len);
    if (!bm->bitmap) {
        fprintf(stderr, "xfs: bitmap allocation failed\n");
        return -1;
    }
    memset(bm->bitmap, 0xFF, bm->bitmap_byte_len);

    /* 3. For each AG, read AGF header and walk the by-block free space B+tree */
    for (uint32_t ag = 0; ag < ag_count; ag++) {
        uint64_t ag_offset = part_offset + (uint64_t)ag * ag_blocks * block_size;
        uint64_t ag_block_base = (uint64_t)ag * ag_blocks;

        /* Read AGF (sector 1 of AG) */
        xfs_agf_t agf;
        uint64_t agf_offset = ag_offset + block_size; /* AGF is at block 1 in the AG */
        /* Actually, AGF is at sector 1 of AG, which may differ from block 1 if blocksize > sectorsize */
        /* For simplicity and correctness: AGF is the 2nd sector of the AG */
        uint16_t sect_size = xfs_be16(sb.sb_sectsize);
        agf_offset = ag_offset + sect_size; /* AGF at sector 1 */

        if (pread(fd, &agf, sizeof(agf), agf_offset) != sizeof(agf)) {
            fprintf(stderr, "xfs: FATAL — failed to read AGF for AG %u\n", ag);
            bitmap_free(bm);
            return -1;
        }
        if (xfs_be32(agf.agf_magicnum) != XFS_AGF_MAGIC) {
            fprintf(stderr, "xfs: FATAL — bad AGF magic for AG %u\n", ag);
            bitmap_free(bm);
            return -1;
        }

        /* Walk the bnobt (free space by-block B+tree) */
        uint32_t bnobt_root = xfs_be32(agf.agf_roots[0]);
        uint16_t bnobt_level = (uint16_t)xfs_be32(agf.agf_levels[0]);

        if (bnobt_root > 0) {
            if (xfs_walk_bnobt(fd, ag_offset, block_size, bnobt_root, bnobt_level,
                               bm->bitmap, ag_block_base, total_blocks) < 0) {
                fprintf(stderr, "xfs: FATAL — failed to read free space btree for AG %u\n", ag);
                bitmap_free(bm);
                return -1;
            }
        }
    }

    /* Count used blocks */
    uint64_t used_count = 0;
    for (uint64_t i = 0; i < bm->bitmap_byte_len; i++) {
        /* popcount each byte */
        uint8_t v = bm->bitmap[i];
        while (v) { used_count += (v & 1); v >>= 1; }
    }
    /* Adjust for trailing bits beyond total_blocks */
    uint32_t trailing = (uint32_t)((bm->bitmap_byte_len * 8) - total_blocks);
    if (trailing > 0 && trailing < 8) {
        uint8_t last = bm->bitmap[bm->bitmap_byte_len - 1];
        for (uint32_t b = (uint32_t)(total_blocks % 8); b < 8; b++) {
            if ((last >> b) & 1) used_count--;
        }
    }
    bm->used_blocks = used_count;

    char total_str[32], used_str[32];
    format_size(total_blocks * block_size, total_str, sizeof(total_str));
    format_size(used_count * block_size, used_str, sizeof(used_str));
    fprintf(stderr, "  xfs bitmap: %llu/%llu blocks used (%s / %s, block_size=%u)\n",
            (unsigned long long)used_count, (unsigned long long)total_blocks,
            used_str, total_str, block_size);
    return 0;
}
