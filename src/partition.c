/*
 * partition.c — partition table parsing (MBR + GPT) and filesystem detection
 */
#include "common.h"
#include <sys/sysmacros.h>

/* ========================================================================= */
/*  MBR structures                                                            */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} mbr_entry_t;

typedef struct {
    uint8_t     bootstrap[446];
    mbr_entry_t entries[4];
    uint16_t    signature;   /* 0xAA55 */
} mbr_t;
#pragma pack(pop)

/* ========================================================================= */
/*  GPT structures                                                            */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    char     signature[8];   /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entry_crc32;
} gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];       /* UTF-16LE */
} gpt_entry_t;
#pragma pack(pop)

/* Known GPT partition type GUIDs — reserved for future use */
/*
static const uint8_t GUID_LINUX_FS[16] = { ... };
static const uint8_t GUID_LINUX_SWAP[16] = { ... };
static const uint8_t GUID_EFI_SYSTEM[16] = { ... };
*/

static bool guid_is_zero(const uint8_t *g)
{
    for (int i = 0; i < 16; i++)
        if (g[i]) return false;
    return true;
}

/* ========================================================================= */
/*  Filesystem magic detection                                                */
/* ========================================================================= */

/* Detect filesystem by reading superblock area from partition start */
static fs_type_t detect_fs(int fd, uint64_t part_offset, char *label, size_t label_len,
                           char *uuid_str, size_t uuid_len)
{
    label[0] = '\0';
    uuid_str[0] = '\0';

    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));

    /* Read first 4KB from partition */
    if (pread(fd, buf, sizeof(buf), part_offset) < 512)
        return FS_UNKNOWN;

    /* ext2/3/4: magic 0xEF53 at offset 1024+56 = 1080 */
    if (sizeof(buf) >= 1082) {
        uint16_t ext_magic = *(uint16_t *)(buf + 1024 + 56);
        if (ext_magic == 0xEF53) {
            /* Determine ext version by feature flags */
            uint32_t compat   = *(uint32_t *)(buf + 1024 + 0x5C);
            uint32_t incompat = *(uint32_t *)(buf + 1024 + 0x60);
            /* Read label (16 bytes at offset 1024+120) */
            memcpy(label, buf + 1024 + 120, 16);
            label[16] = '\0';
            /* Read UUID (16 bytes at offset 1024+104) */
            uint8_t *u = buf + 1024 + 104;
            snprintf(uuid_str, uuid_len,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
                u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);

            if (incompat & 0x0040) return FS_EXT4;  /* INCOMPAT_EXTENTS */
            if (compat & 0x0004)   return FS_EXT3;  /* COMPAT_HAS_JOURNAL */
            return FS_EXT2;
        }
    }

    /* XFS: magic "XFSB" at offset 0 */
    if (memcmp(buf, "XFSB", 4) == 0) {
        /* XFS superblock: label at offset 108 (12 bytes) */
        memcpy(label, buf + 108, 12);
        label[12] = '\0';
        /* UUID at offset 32 (16 bytes, big-endian) */
        uint8_t *u = buf + 32;
        snprintf(uuid_str, uuid_len,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
            u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
        return FS_XFS;
    }

    /* Btrfs: magic "_BHRfS_M" at offset 0x10040 — need to read further */
    {
        uint8_t btr[16];
        if (pread(fd, btr, 16, part_offset + 0x10040) == 16) {
            if (memcmp(btr, "_BHRfS_M", 8) == 0)
                return FS_BTRFS;
        }
    }

    /* Linux swap: magic at offset 4086 or end-of-page */
    {
        uint8_t sw[10];
        if (pread(fd, sw, 10, part_offset + 4086) == 10) {
            if (memcmp(sw, "SWAPSPACE2", 10) == 0 || memcmp(sw, "SWAP-SPACE", 10) == 0)
                return FS_SWAP;
        }
    }

    /* FAT32: check for FAT signature */
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        if (memcmp(buf + 82, "FAT32   ", 8) == 0)
            return FS_FAT32;
    }

    /* NTFS */
    if (memcmp(buf + 3, "NTFS", 4) == 0)
        return FS_NTFS;

    /* LVM2 Physical Volume: "LABELONE" at sector 1 (offset 512) */
    {
        uint8_t lvm[512];
        if (pread(fd, lvm, 512, part_offset + 512) == 512) {
            if (memcmp(lvm, "LABELONE", 8) == 0)
                return FS_LVM;
        }
    }

    return FS_UNKNOWN;
}

const char *fs_type_name(fs_type_t t)
{
    switch (t) {
    case FS_EXT4:    return "ext4";
    case FS_EXT3:    return "ext3";
    case FS_EXT2:    return "ext2";
    case FS_XFS:     return "xfs";
    case FS_BTRFS:   return "btrfs";
    case FS_SWAP:    return "swap";
    case FS_NTFS:    return "ntfs";
    case FS_FAT32:   return "fat32";
    case FS_LVM:     return "lvm";
    case FS_OTHER:   return "other";
    default:         return "unknown";
    }
}

/* ========================================================================= */
/*  Partition scanning                                                        */
/* ========================================================================= */

int partition_scan(int fd, disk_info_t *info)
{
    info->num_partitions = 0;
    info->pt_type = PT_UNKNOWN;

    /* Copy disk dev_path to a local buffer to avoid snprintf overlap warnings */
    char disk_dev[MAX_PATH_LEN];
    strncpy(disk_dev, info->dev_path, sizeof(disk_dev) - 1);
    disk_dev[sizeof(disk_dev) - 1] = '\0';

    /* Determine partition naming: nvme/mmcblk use "p" separator */
    const char *dbase = strrchr(disk_dev, '/');
    if (dbase) dbase++; else dbase = disk_dev;
    bool needs_p = (strncmp(dbase, "nvme", 4) == 0 || strncmp(dbase, "mmcblk", 6) == 0);

    uint8_t sector0[512];
    if (pread(fd, sector0, 512, 0) != 512)
        return -1;

    /* Check for GPT first (LBA 1) */
    uint8_t gpt_buf[512];
    if (pread(fd, gpt_buf, 512, 512) == 512) {
        gpt_header_t *gh = (gpt_header_t *)gpt_buf;
        if (memcmp(gh->signature, "EFI PART", 8) == 0) {
            info->pt_type = PT_GPT;

            uint32_t entry_count = gh->num_partition_entries;
            uint32_t entry_size  = gh->partition_entry_size;
            if (entry_count > MAX_PARTITIONS) entry_count = MAX_PARTITIONS;
            if (entry_size < 128) entry_size = 128;

            size_t tbl_size = (size_t)entry_count * entry_size;
            uint8_t *tbl = (uint8_t *)malloc(tbl_size);
            if (!tbl) return -1;

            if (pread(fd, tbl, tbl_size, gh->partition_entry_lba * 512) != (ssize_t)tbl_size) {
                free(tbl);
                return -1;
            }

            int pnum = 0;
            for (uint32_t i = 0; i < entry_count && pnum < MAX_PARTITIONS; i++) {
                gpt_entry_t *e = (gpt_entry_t *)(tbl + i * entry_size);
                if (guid_is_zero(e->type_guid)) continue;
                if (e->starting_lba == 0 || e->ending_lba == 0) continue;

                partition_info_t *p = &info->partitions[pnum];
                p->number = i + 1;  /* GPT entry index, matches kernel numbering */
                p->offset = e->starting_lba * 512;
                p->size   = (e->ending_lba - e->starting_lba + 1) * 512;
                p->selected = true;
                p->copy_mode = 0;

                /* Build device path */
                if (needs_p)
                    snprintf(p->dev_path, sizeof(p->dev_path), "%sp%d", disk_dev, (int)(i + 1));
                else
                    snprintf(p->dev_path, sizeof(p->dev_path), "%s%d", disk_dev, (int)(i + 1));

                /* Detect filesystem */
                p->fs_type = detect_fs(fd, p->offset, p->fs_label,
                                       sizeof(p->fs_label), p->fs_uuid, sizeof(p->fs_uuid));
                pnum++;
            }
            info->num_partitions = pnum;
            free(tbl);
            goto resolve_mounts;
        }
    }

    /* Fall back to MBR */
    mbr_t *mbr = (mbr_t *)sector0;
    if (mbr->signature != 0xAA55) {
        /* No partition table. Check if whole disk has a filesystem. */
        partition_info_t *p = &info->partitions[0];
        p->number = 0;
        p->offset = 0;
        p->size   = info->size;
        p->selected = true;
        p->copy_mode = 0;
        strncpy(p->dev_path, disk_dev, sizeof(p->dev_path) - 1);
        p->fs_type = detect_fs(fd, 0, p->fs_label, sizeof(p->fs_label),
                               p->fs_uuid, sizeof(p->fs_uuid));
        if (p->fs_type != FS_UNKNOWN) {
            info->num_partitions = 1;
        }
        goto resolve_mounts;
    }

    info->pt_type = PT_MBR;
    int pnum = 0;
    uint64_t ebr_base = 0;  /* LBA of the extended partition start */

    /* First pass: primary partitions, and find the extended partition */
    for (int i = 0; i < 4 && pnum < MAX_PARTITIONS; i++) {
        mbr_entry_t *e = &mbr->entries[i];
        if (e->type == 0 || e->lba_count == 0) continue;

        /* Extended partition — remember its start for EBR chain walk */
        if (e->type == 0x05 || e->type == 0x0F || e->type == 0x85) {
            ebr_base = (uint64_t)e->lba_start;
            continue;
        }

        partition_info_t *p = &info->partitions[pnum];
        p->number = i + 1;   /* MBR slot number: 1-4 */
        p->offset = (uint64_t)e->lba_start * 512;
        p->size   = (uint64_t)e->lba_count * 512;
        p->selected = true;
        p->copy_mode = 0;

        const char *base = strrchr(disk_dev, '/');
        if (base) base++; else base = disk_dev;
        if (strncmp(base, "nvme", 4) == 0 || strncmp(base, "mmcblk", 6) == 0)
            snprintf(p->dev_path, sizeof(p->dev_path), "%sp%d", disk_dev, i + 1);
        else
            snprintf(p->dev_path, sizeof(p->dev_path), "%s%d", disk_dev, i + 1);

        p->fs_type = detect_fs(fd, p->offset, p->fs_label,
                               sizeof(p->fs_label), p->fs_uuid, sizeof(p->fs_uuid));
        pnum++;
    }

    /* Second pass: walk EBR chain for logical partitions (5, 6, 7, ...) */
    if (ebr_base > 0) {
        uint64_t ebr_lba = ebr_base;
        int logical_num = 5;  /* Linux logical partitions start at 5 */

        while (ebr_lba > 0 && pnum < MAX_PARTITIONS) {
            uint8_t ebr_buf[512];
            if (pread(fd, ebr_buf, 512, ebr_lba * 512) != 512)
                break;

            /* EBR has same structure as MBR: 4 entries at offset 446, sig at 510 */
            if (ebr_buf[510] != 0x55 || ebr_buf[511] != 0xAA)
                break;

            mbr_entry_t *entries = (mbr_entry_t *)(ebr_buf + 446);

            /* Entry 0: the logical partition (offset relative to THIS EBR) */
            if (entries[0].type != 0 && entries[0].lba_count > 0) {
                uint64_t part_lba = ebr_lba + entries[0].lba_start;

                partition_info_t *p = &info->partitions[pnum];
                p->number = logical_num;
                p->offset = part_lba * 512;
                p->size   = (uint64_t)entries[0].lba_count * 512;
                p->selected = true;
                p->copy_mode = 0;

                const char *base = strrchr(disk_dev, '/');
                if (base) base++; else base = disk_dev;
                if (strncmp(base, "nvme", 4) == 0 || strncmp(base, "mmcblk", 6) == 0)
                    snprintf(p->dev_path, sizeof(p->dev_path), "%sp%d", disk_dev, logical_num);
                else
                    snprintf(p->dev_path, sizeof(p->dev_path), "%s%d", disk_dev, logical_num);

                p->fs_type = detect_fs(fd, p->offset, p->fs_label,
                                       sizeof(p->fs_label), p->fs_uuid, sizeof(p->fs_uuid));
                pnum++;
                logical_num++;
            }

            /* Entry 1: pointer to next EBR (offset relative to EXTENDED partition start) */
            if (entries[1].type != 0 && entries[1].lba_start > 0)
                ebr_lba = ebr_base + entries[1].lba_start;
            else
                ebr_lba = 0;  /* end of chain */
        }
    }

    info->num_partitions = pnum;

resolve_mounts:
    /* Resolve mountpoints from /proc/mounts.
     * For LVM partitions, also check /dev/mapper/ and /dev/dm- entries
     * to find mountpoints of LVs that reside on this PV. */
    {
        int nparts = info->num_partitions;
        FILE *fp = fopen("/proc/mounts", "r");
        if (fp) {
            char mline[1024];
            while (fgets(mline, sizeof(mline), fp)) {
                char mdev[512], mpoint[512];
                if (sscanf(mline, "%511s %511s", mdev, mpoint) != 2) continue;

                int pi;
                for (pi = 0; pi < nparts; pi++) {
                    partition_info_t *p = &info->partitions[pi];
                    if (p->mountpoint[0]) continue; /* already found */

                    /* Direct match: /dev/sda1 mounted at /boot */
                    if (strcmp(mdev, p->dev_path) == 0) {
                        strncpy(p->mountpoint, mpoint, sizeof(p->mountpoint) - 1);
                    }
                }
            }
            fclose(fp);
        }

        /* For LVM partitions: find dm device mountpoints. */
        {
            int pi;
            for (pi = 0; pi < nparts; pi++) {
                partition_info_t *p = &info->partitions[pi];
                if (p->fs_type != FS_LVM) continue;

                /* Use dmsetup table to find LVs on this partition */
                FILE *dmfp = popen("dmsetup table 2>/dev/null", "r");
                if (!dmfp) continue;

                struct stat pst;
                if (stat(p->dev_path, &pst) < 0) { pclose(dmfp); continue; }

                char dline[1024];
                char lv_mounts[MAX_PATH_LEN];
                int lv_mounts_len = 0;
                lv_mounts[0] = '\0';

                while (fgets(dline, sizeof(dline), dmfp)) {
                    char dmname[128];
                    uint64_t d1, d2;
                    char mtype[32];
                    unsigned dmaj, dmin;
                    uint64_t d3;

                    char *colon = strchr(dline, ':');
                    if (!colon) continue;
                    size_t nlen = (size_t)(colon - dline);
                    if (nlen >= sizeof(dmname)) nlen = sizeof(dmname) - 1;
                    memcpy(dmname, dline, nlen);
                    dmname[nlen] = '\0';

                    if (sscanf(colon + 1, " %llu %llu %31s %u:%u %llu",
                               (unsigned long long *)&d1, (unsigned long long *)&d2,
                               mtype, &dmaj, &dmin, (unsigned long long *)&d3) != 6)
                        continue;
                    if (strcmp(mtype, "linear") != 0) continue;
                    if (makedev(dmaj, dmin) != pst.st_rdev) continue;

                    /* This LV is on our partition. Find its mountpoint. */
                    char lv_dev[MAX_PATH_LEN];
                    snprintf(lv_dev, sizeof(lv_dev), "/dev/mapper/%s", dmname);

                    FILE *mfp = fopen("/proc/mounts", "r");
                    if (!mfp) continue;
                    char ml2[1024];
                    while (fgets(ml2, sizeof(ml2), mfp)) {
                        char md[512], mp[512];
                        if (sscanf(ml2, "%511s %511s", md, mp) != 2) continue;
                        if (strcmp(md, lv_dev) == 0) {
                            /* Append to lv_mounts */
                            int slen = (int)strlen(mp);
                            if (lv_mounts_len > 0 && lv_mounts_len + slen + 1 < MAX_PATH_LEN) {
                                lv_mounts[lv_mounts_len++] = ',';
                            }
                            if (lv_mounts_len + slen < MAX_PATH_LEN) {
                                memcpy(lv_mounts + lv_mounts_len, mp, slen);
                                lv_mounts_len += slen;
                                lv_mounts[lv_mounts_len] = '\0';
                            }
                            break;
                        }
                    }
                    fclose(mfp);

                    /* Check if it's swap */
                    {
                        FILE *sfp = fopen("/proc/swaps", "r");
                        if (sfp) {
                            char sl[1024];
                            while (fgets(sl, sizeof(sl), sfp)) {
                                if (strncmp(sl, lv_dev, strlen(lv_dev)) == 0) {
                                    const char *stag = "[SWAP]";
                                    int slen = (int)strlen(stag);
                                    if (lv_mounts_len > 0 && lv_mounts_len + slen + 1 < MAX_PATH_LEN)
                                        lv_mounts[lv_mounts_len++] = ',';
                                    if (lv_mounts_len + slen < MAX_PATH_LEN) {
                                        memcpy(lv_mounts + lv_mounts_len, stag, slen);
                                        lv_mounts_len += slen;
                                        lv_mounts[lv_mounts_len] = '\0';
                                    }
                                    break;
                                }
                            }
                            fclose(sfp);
                        }
                    }
                }
                pclose(dmfp);

                if (lv_mounts_len > 0)
                    strncpy(p->mountpoint, lv_mounts, sizeof(p->mountpoint) - 1);
            }
        }
    }

    return 0;
}
