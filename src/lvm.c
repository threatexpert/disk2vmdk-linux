/*
 * lvm.c — LVM awareness for disk imaging
 *
 * When a partition is LVM PV and --used-only is requested:
 *   1. Run `dmsetup table` to find LVs mapped to this PV (by major:minor)
 *   2. Open each LV device (/dev/mapper/xxx), detect filesystem, read bitmap
 *   3. Translate LV bitmap → partition-relative physical offsets
 *   4. Merge all LVs' bitmaps into one partition-level bitmap
 *
 * Requires: device-mapper active, VG activated (vgchange -ay).
 *
 * dmsetup table output format:
 *   cs-root: 0 35643392 linear 259:2 4196352
 *   means: LV "cs-root", LV sectors 0..35643392, linear map to dev 259:2,
 *          starting at sector 4196352 on that device.
 */
#include "common.h"
#include <dirent.h>
#include <sys/sysmacros.h>

/* Get major:minor of a device path as a string "major:minor" */
static int get_dev_majmin(const char *dev_path, unsigned *out_maj, unsigned *out_min)
{
    struct stat st;
    if (stat(dev_path, &st) < 0) return -1;
    if (!S_ISBLK(st.st_mode)) return -1;
    *out_maj = major(st.st_rdev);
    *out_min = minor(st.st_rdev);
    return 0;
}

/* Detect filesystem type on a device by reading its first bytes */
static fs_type_t detect_fs_on_dev(const char *dev_path, char *label, char *uuid_str)
{
    label[0] = '\0';
    uuid_str[0] = '\0';

    int fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd < 0) return FS_UNKNOWN;

    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, sizeof(buf), 0) < 512) {
        close(fd);
        return FS_UNKNOWN;
    }

    fs_type_t result = FS_UNKNOWN;

    /* ext2/3/4: magic 0xEF53 at offset 1024+56 */
    if (sizeof(buf) >= 1082) {
        uint16_t ext_magic = *(uint16_t *)(buf + 1024 + 56);
        if (ext_magic == 0xEF53) {
            uint32_t incompat = *(uint32_t *)(buf + 1024 + 0x60);
            uint32_t compat   = *(uint32_t *)(buf + 1024 + 0x5C);
            memcpy(label, buf + 1024 + 120, 16);
            label[16] = '\0';
            uint8_t *u = buf + 1024 + 104;
            sprintf(uuid_str,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
                u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
            if (incompat & 0x0040) result = FS_EXT4;
            else if (compat & 0x0004) result = FS_EXT3;
            else result = FS_EXT2;
            close(fd);
            return result;
        }
    }

    /* XFS */
    if (memcmp(buf, "XFSB", 4) == 0) {
        memcpy(label, buf + 108, 12);
        label[12] = '\0';
        uint8_t *u = buf + 32;
        sprintf(uuid_str,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
            u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
        close(fd);
        return FS_XFS;
    }

    /* Swap */
    {
        uint8_t sw[10];
        if (pread(fd, sw, 10, 4086) == 10) {
            if (memcmp(sw, "SWAPSPACE2", 10) == 0 || memcmp(sw, "SWAP-SPACE", 10) == 0) {
                close(fd);
                return FS_SWAP;
            }
        }
    }

    close(fd);
    return FS_UNKNOWN;
}

/* ========================================================================= */
/*  Scan LVs on a given PV partition                                          */
/* ========================================================================= */

int lvm_scan_lvs(const char *part_dev_path, lvm_scan_result_t *result)
{
    memset(result, 0, sizeof(*result));

    /* Get target PV's major:minor */
    unsigned pv_major, pv_minor;
    if (get_dev_majmin(part_dev_path, &pv_major, &pv_minor) < 0) {
        fprintf(stderr, "lvm: cannot stat %s\n", part_dev_path);
        return -1;
    }

    /* Run dmsetup table to get all DM mappings */
    FILE *fp = popen("dmsetup table 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "lvm: dmsetup not available\n");
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) && result->num_lvs < MAX_LVS) {
        /* Parse: "cs-root: 0 35643392 linear 259:2 4196352" */
        char name[128];
        uint64_t lv_start, lv_sectors;
        char map_type[32];
        unsigned dev_maj, dev_min;
        uint64_t pv_sector;

        /* Extract DM name (before the colon) */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        size_t nlen = (size_t)(colon - line);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, line, nlen);
        name[nlen] = '\0';

        /* Parse the rest */
        if (sscanf(colon + 1, " %llu %llu %31s %u:%u %llu",
                   (unsigned long long *)&lv_start,
                   (unsigned long long *)&lv_sectors,
                   map_type, &dev_maj, &dev_min,
                   (unsigned long long *)&pv_sector) != 6)
            continue;

        /* Only handle "linear" mappings to our PV */
        if (strcmp(map_type, "linear") != 0) continue;
        if (dev_maj != pv_major || dev_min != pv_minor) continue;

        lv_info_t *lv = &result->lvs[result->num_lvs];
        strncpy(lv->dm_name, name, sizeof(lv->dm_name) - 1);
        snprintf(lv->dev_path, sizeof(lv->dev_path), "/dev/mapper/%s", name);
        lv->lv_sectors = lv_sectors;
        lv->pv_start_sector = pv_sector;

        /* Detect filesystem on the LV */
        lv->fs_type = detect_fs_on_dev(lv->dev_path, lv->fs_label, lv->fs_uuid);

        result->num_lvs++;
    }
    pclose(fp);

    if (result->num_lvs == 0) {
        fprintf(stderr, "lvm: no active LVs found on %s\n", part_dev_path);
        return -1;
    }

    return 0;
}

/* ========================================================================= */
/*  Build combined bitmap for LVM partition                                   */
/* ========================================================================= */

int lvm_build_bitmap(const char *part_dev_path,
                     uint64_t part_offset, uint64_t part_size,
                     block_bitmap_t *bm)
{
    /* Scan LVs */
    lvm_scan_result_t scan;
    if (lvm_scan_lvs(part_dev_path, &scan) < 0)
        return -1;

    /* The partition bitmap uses 512-byte sectors as the unit.
     * bit=1 means "used" (copy this sector), bit=0 means "free" (skip).
     * Start with all-zero (all free). Mark LVM metadata area and used
     * filesystem blocks as used. */
    uint64_t part_sectors = part_size / 512;
    bm->total_blocks = part_sectors;
    bm->block_size = 512;
    bm->bitmap_byte_len = (part_sectors + 7) / 8;
    bm->bitmap = (uint8_t *)calloc(1, bm->bitmap_byte_len);
    if (!bm->bitmap) return -1;

    /* Mark LVM metadata area as used (first 1MB typically) */
    /* Find the lowest pv_start_sector among all LVs to determine metadata end */
    uint64_t meta_end = 2048; /* default: 1MB / 512 = 2048 sectors */
    uint64_t i;
    int lv_idx;
    for (lv_idx = 0; lv_idx < scan.num_lvs; lv_idx++) {
        if (scan.lvs[lv_idx].pv_start_sector < meta_end)
            meta_end = scan.lvs[lv_idx].pv_start_sector;
    }
    if (meta_end > 0) {
        /* Mark sectors 0..meta_end-1 as used */
        for (i = 0; i < meta_end && i < part_sectors; i++)
            bm->bitmap[i / 8] |= (1u << (i % 8));
    }

    fprintf(stderr, "    LVM: %d LVs on %s (metadata: %llu sectors)\n",
            scan.num_lvs, part_dev_path, (unsigned long long)meta_end);

    /* For each LV, read its filesystem bitmap and map back */
    for (lv_idx = 0; lv_idx < scan.num_lvs; lv_idx++) {
        lv_info_t *lv = &scan.lvs[lv_idx];

        fprintf(stderr, "    LV %-20s  %-8s  %s\n",
                lv->dm_name, fs_type_name(lv->fs_type), lv->fs_label);

        if (lv->fs_type == FS_SWAP) {
            /* Swap: mark first 4KB as used (swap header), rest skip */
            uint64_t swap_sectors = 8; /* 4KB */
            uint64_t s;
            for (s = 0; s < swap_sectors && s < lv->lv_sectors; s++) {
                uint64_t ps = lv->pv_start_sector + s;
                if (ps < part_sectors)
                    bm->bitmap[ps / 8] |= (1u << (ps % 8));
            }
            fprintf(stderr, "      swap: skipping (only header marked)\n");
            continue;
        }

        if (lv->fs_type != FS_EXT4 && lv->fs_type != FS_EXT3 &&
            lv->fs_type != FS_EXT2 && lv->fs_type != FS_XFS) {
            /* Unknown FS: mark entire LV as used for safety */
            uint64_t s;
            for (s = 0; s < lv->lv_sectors; s++) {
                uint64_t ps = lv->pv_start_sector + s;
                if (ps < part_sectors)
                    bm->bitmap[ps / 8] |= (1u << (ps % 8));
            }
            fprintf(stderr, "      unknown fs, marking all as used\n");
            continue;
        }

        /* Read filesystem bitmap from the LV device */
        int lv_fd = open(lv->dev_path, O_RDONLY | O_LARGEFILE);
        if (lv_fd < 0) {
            fprintf(stderr, "      WARNING: cannot open %s, marking all used\n", lv->dev_path);
            uint64_t s;
            for (s = 0; s < lv->lv_sectors; s++) {
                uint64_t ps = lv->pv_start_sector + s;
                if (ps < part_sectors)
                    bm->bitmap[ps / 8] |= (1u << (ps % 8));
            }
            continue;
        }

        block_bitmap_t fs_bm;
        int bm_rc = -1;
        if (lv->fs_type == FS_EXT4 || lv->fs_type == FS_EXT3 || lv->fs_type == FS_EXT2)
            bm_rc = ext4_read_bitmap(lv_fd, 0, lv->lv_sectors * 512, &fs_bm);
        else if (lv->fs_type == FS_XFS)
            bm_rc = xfs_read_bitmap(lv_fd, 0, lv->lv_sectors * 512, &fs_bm);
        close(lv_fd);

        if (bm_rc < 0) {
            fprintf(stderr, "      WARNING: bitmap read failed, marking all used\n");
            uint64_t s;
            for (s = 0; s < lv->lv_sectors; s++) {
                uint64_t ps = lv->pv_start_sector + s;
                if (ps < part_sectors)
                    bm->bitmap[ps / 8] |= (1u << (ps % 8));
            }
            continue;
        }

        /* Map FS bitmap back to partition sectors.
         * FS block N (at LV byte offset N*fs_bm.block_size) maps to
         * PV sector: lv->pv_start_sector + (N * fs_bm.block_size / 512) */
        uint32_t sectors_per_fsblock = fs_bm.block_size / 512;
        uint64_t blk;
        for (blk = 0; blk < fs_bm.total_blocks; blk++) {
            if (bitmap_is_used(&fs_bm, blk)) {
                /* Mark all 512-byte sectors of this FS block as used */
                uint64_t base_sector = lv->pv_start_sector + blk * sectors_per_fsblock;
                uint32_t s;
                for (s = 0; s < sectors_per_fsblock; s++) {
                    uint64_t ps = base_sector + s;
                    if (ps < part_sectors)
                        bm->bitmap[ps / 8] |= (1u << (ps % 8));
                }
            }
        }
        bitmap_free(&fs_bm);
    }

    /* Count used sectors */
    uint64_t used = 0;
    for (i = 0; i < bm->bitmap_byte_len; i++) {
        uint8_t v = bm->bitmap[i];
        while (v) { used += (v & 1); v >>= 1; }
    }
    /* Adjust trailing bits */
    uint32_t trailing = (uint32_t)((bm->bitmap_byte_len * 8) - part_sectors);
    if (trailing > 0 && trailing < 8) {
        uint8_t last = bm->bitmap[bm->bitmap_byte_len - 1];
        uint32_t b;
        for (b = (uint32_t)(part_sectors % 8); b < 8; b++) {
            if ((last >> b) & 1) used--;
        }
    }
    bm->used_blocks = used;

    char total_str[32], used_str[32];
    format_size(part_sectors * 512, total_str, sizeof(total_str));
    format_size(used * 512, used_str, sizeof(used_str));
    fprintf(stderr, "    LVM combined: %llu/%llu sectors used (%s / %s)\n",
            (unsigned long long)used, (unsigned long long)part_sectors,
            used_str, total_str);
    return 0;
}
