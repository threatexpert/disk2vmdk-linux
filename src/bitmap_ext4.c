/*
 * bitmap_ext4.c — read ext4/ext3/ext2 block allocation bitmap
 *
 * Reads the on-disk structures directly (no libext2fs dependency):
 *   1. Read superblock at partition_offset + 1024
 *   2. Read group descriptor table
 *   3. For each block group, read its block bitmap
 *   4. Assemble into a single flat bitmap array
 *
 * All superblock/GDT fields are read by fixed offset from raw buffers
 * to avoid any struct packing/padding issues.
 */
#include "common.h"

/* Read little-endian values from raw buffer */
static inline uint16_t rd16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

/* Superblock field offsets (relative to superblock start) */
#define SB_BLOCKS_COUNT_LO    0x04
#define SB_FIRST_DATA_BLOCK   0x14
#define SB_LOG_BLOCK_SIZE     0x18
#define SB_BLOCKS_PER_GROUP   0x20
#define SB_MAGIC              0x38
#define SB_FEATURE_INCOMPAT   0x60
#define SB_DESC_SIZE          0xFE
#define SB_BLOCKS_COUNT_HI    0x150

/* Group descriptor field offsets (relative to descriptor start) */
#define GD_BLOCK_BITMAP_LO    0x00
#define GD_FLAGS              0x12
#define GD_BLOCK_BITMAP_HI    0x20  /* only in 64-byte descriptors */

/* Feature flag */
#define EXT4_FEATURE_INCOMPAT_64BIT  0x0080

/* Group descriptor flag */
#define EXT4_BG_BLOCK_UNINIT  0x0002

void bitmap_free(block_bitmap_t *bm)
{
    free(bm->bitmap);
    memset(bm, 0, sizeof(*bm));
}

int ext4_read_bitmap(int fd, uint64_t part_offset, uint64_t part_size,
                     block_bitmap_t *bm)
{
    memset(bm, 0, sizeof(*bm));

    /* 1. Read superblock (1024 bytes at partition_offset + 1024) */
    uint8_t sb_buf[1024];
    if (pread(fd, sb_buf, 1024, part_offset + 1024) != 1024) {
        fprintf(stderr, "ext4: failed to read superblock\n");
        return -1;
    }
    if (rd16(sb_buf + SB_MAGIC) != 0xEF53) {
        fprintf(stderr, "ext4: bad magic 0x%04X\n", rd16(sb_buf + SB_MAGIC));
        return -1;
    }

    uint32_t log_block_size = rd32(sb_buf + SB_LOG_BLOCK_SIZE);
    uint32_t block_size = 1024u << log_block_size;
    uint32_t blocks_per_group = rd32(sb_buf + SB_BLOCKS_PER_GROUP);
    uint32_t incompat = rd32(sb_buf + SB_FEATURE_INCOMPAT);
    bool is_64bit = (incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;

    uint32_t desc_size;
    if (is_64bit) {
        desc_size = rd16(sb_buf + SB_DESC_SIZE);
        if (desc_size < 64) desc_size = 64;  /* minimum for 64bit mode */
    } else {
        desc_size = 32;
    }

    uint64_t total_blocks;
    if (is_64bit)
        total_blocks = ((uint64_t)rd32(sb_buf + SB_BLOCKS_COUNT_HI) << 32) | rd32(sb_buf + SB_BLOCKS_COUNT_LO);
    else
        total_blocks = rd32(sb_buf + SB_BLOCKS_COUNT_LO);

    uint32_t num_groups = (uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group);

    /* 2. Allocate output bitmap */
    bm->total_blocks = total_blocks;
    bm->block_size = block_size;
    bm->bitmap_byte_len = (total_blocks + 7) / 8;
    bm->bitmap = (uint8_t *)calloc(1, bm->bitmap_byte_len);
    if (!bm->bitmap) {
        fprintf(stderr, "ext4: bitmap allocation failed (%llu bytes)\n",
                (unsigned long long)bm->bitmap_byte_len);
        return -1;
    }

    /* 3. Read group descriptor table */
    uint64_t gdt_block = (block_size > 1024) ? 1 : 2;
    uint64_t gdt_offset = part_offset + gdt_block * block_size;
    size_t gdt_size = (size_t)num_groups * desc_size;
    uint8_t *gdt = (uint8_t *)malloc(gdt_size);
    if (!gdt) { bitmap_free(bm); return -1; }

    if (pread(fd, gdt, gdt_size, gdt_offset) != (ssize_t)gdt_size) {
        fprintf(stderr, "ext4: failed to read GDT (%u groups, desc_size=%u, 64bit=%d)\n",
                num_groups, desc_size, is_64bit);
        free(gdt); bitmap_free(bm);
        return -1;
    }

    /* 4. Read each block group's bitmap */
    uint8_t *one_bm = (uint8_t *)malloc(block_size);
    if (!one_bm) { free(gdt); bitmap_free(bm); return -1; }

    uint64_t used_count = 0;
    uint64_t block_idx = 0;
    uint32_t g;

    for (g = 0; g < num_groups; g++) {
        uint8_t *desc = gdt + (size_t)g * desc_size;

        uint32_t blocks_this_group = blocks_per_group;
        if (block_idx + blocks_per_group > total_blocks)
            blocks_this_group = (uint32_t)(total_blocks - block_idx);

        /* Check BLOCK_UNINIT flag */
        uint16_t flags = rd16(desc + GD_FLAGS);
        if (flags & EXT4_BG_BLOCK_UNINIT) {
            block_idx += blocks_this_group;
            continue;
        }

        /* Get block bitmap location */
        uint64_t bm_block = rd32(desc + GD_BLOCK_BITMAP_LO);
        if (is_64bit && desc_size >= 64)
            bm_block |= ((uint64_t)rd32(desc + GD_BLOCK_BITMAP_HI) << 32);

        uint64_t bm_offset = part_offset + bm_block * block_size;
        if (pread(fd, one_bm, block_size, bm_offset) != (ssize_t)block_size) {
            fprintf(stderr, "ext4: FATAL — failed to read bitmap for group %u (block %llu)\n",
                    g, (unsigned long long)bm_block);
            fprintf(stderr, "ext4: bitmap data is unreliable, aborting used-only mode\n");
            free(one_bm); free(gdt); bitmap_free(bm);
            return -1;
        }

        /* Copy bits into output bitmap */
        uint32_t full_bytes = blocks_this_group / 8;
        uint32_t leftover_bits = blocks_this_group % 8;
        uint64_t out_byte = block_idx / 8;
        uint32_t out_bit  = (uint32_t)(block_idx % 8);

        if (out_bit == 0) {
            memcpy(bm->bitmap + out_byte, one_bm, full_bytes);
            if (leftover_bits > 0) {
                uint8_t mask = (uint8_t)((1u << leftover_bits) - 1);
                bm->bitmap[out_byte + full_bytes] |= (one_bm[full_bytes] & mask);
            }
        } else {
            uint32_t b;
            for (b = 0; b < blocks_this_group; b++) {
                if ((one_bm[b / 8] >> (b % 8)) & 1) {
                    uint64_t gb = block_idx + b;
                    bm->bitmap[gb / 8] |= (1u << (gb % 8));
                }
            }
        }

        /* Count used blocks */
        {
            uint32_t b;
            for (b = 0; b < blocks_this_group; b++) {
                if ((one_bm[b / 8] >> (b % 8)) & 1)
                    used_count++;
            }
        }

        block_idx += blocks_this_group;
    }

    bm->used_blocks = used_count;
    free(one_bm);
    free(gdt);

    char total_str[32], used_str[32];
    format_size(total_blocks * block_size, total_str, sizeof(total_str));
    format_size(used_count * block_size, used_str, sizeof(used_str));
    fprintf(stderr, "  ext4 bitmap: %llu/%llu blocks used (%s / %s, block_size=%u, desc_size=%u)\n",
            (unsigned long long)used_count, (unsigned long long)total_blocks,
            used_str, total_str, block_size, desc_size);
    return 0;
}
