/*
 * disk2vmdk — Linux disk imaging tool
 * Common header: types, error handling, module interfaces
 */
#ifndef D2V_COMMON_H
#define D2V_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>

/* Define BLKGETSIZE64/BLKSSZGET directly to avoid linux/fs.h vs sys/mount.h conflict */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#define BLKSSZGET    _IO(0x12, 104)
#endif

/* ========================================================================= */
/*  Constants                                                                 */
/* ========================================================================= */

#define SECTOR_SIZE         512
#define MAX_PARTITIONS      128
#define MAX_PATH_LEN        1024

/* ========================================================================= */
/*  Partition table types                                                     */
/* ========================================================================= */

typedef enum {
    PT_UNKNOWN = 0,
    PT_MBR,
    PT_GPT
} part_table_type_t;

typedef enum {
    FS_UNKNOWN = 0,
    FS_EXT4,
    FS_EXT3,
    FS_EXT2,
    FS_XFS,
    FS_BTRFS,
    FS_SWAP,
    FS_NTFS,
    FS_FAT32,
    FS_LVM,
    FS_OTHER
} fs_type_t;

/* ========================================================================= */
/*  Partition info                                                            */
/* ========================================================================= */

typedef struct {
    int          number;          /* partition number (1-based) */
    uint64_t     offset;          /* byte offset on disk */
    uint64_t     size;            /* size in bytes */
    fs_type_t    fs_type;         /* detected filesystem */
    char         fs_label[64];    /* filesystem label */
    char         fs_uuid[48];     /* filesystem UUID */
    char         dev_path[MAX_PATH_LEN]; /* e.g. /dev/sda1 */

    /* user selection */
    bool         selected;        /* include in image? */
    int          copy_mode;       /* 0=full space, 1=used only */
} partition_info_t;

/* ========================================================================= */
/*  Disk info                                                                 */
/* ========================================================================= */

typedef struct {
    char              dev_path[MAX_PATH_LEN]; /* e.g. /dev/sda */
    uint64_t          size;                   /* total disk size in bytes */
    uint32_t          sector_size;            /* logical sector size */
    part_table_type_t pt_type;                /* MBR or GPT */
    char              model[128];             /* disk model string */

    int               num_partitions;
    partition_info_t  partitions[MAX_PARTITIONS];
} disk_info_t;

/* ========================================================================= */
/*  Bitmap (used-block tracking for a partition)                              */
/* ========================================================================= */

typedef struct {
    uint64_t     total_blocks;     /* total blocks in filesystem */
    uint32_t     block_size;       /* filesystem block size in bytes */
    uint64_t     used_blocks;      /* number of used blocks */
    uint8_t     *bitmap;           /* bit array: 1=used, 0=free */
    uint64_t     bitmap_byte_len;  /* byte length of bitmap array */
} block_bitmap_t;

/* ========================================================================= */
/*  VDisk writer (VMDK / VHD / VDI)                                          */
/* ========================================================================= */

typedef enum {
    VDISK_FMT_DD = 0,
    VDISK_FMT_VMDK,
    VDISK_FMT_VHD,
    VDISK_FMT_VDI,
} vdisk_format_t;

typedef struct vdisk_writer vdisk_writer_t;

/* ========================================================================= */
/*  Imaging callback                                                          */
/* ========================================================================= */

typedef void (*progress_cb_t)(uint64_t bytes_done, uint64_t bytes_total,
                              uint64_t data_written, double speed_mbps);

/* ========================================================================= */
/*  Module: disk.c — disk enumeration and raw IO                              */
/* ========================================================================= */

int  disk_open(const char *dev_path, disk_info_t *info);
void disk_close(int fd);
int  disk_list_all(void);                   /* list all disks (summary) */
int  disk_list_one(const char *dev_path);   /* list one disk (detailed partitions) */

/* ========================================================================= */
/*  Module: partition.c — partition table + filesystem detection               */
/* ========================================================================= */

int  partition_scan(int fd, disk_info_t *info);
const char *fs_type_name(fs_type_t t);

/* ========================================================================= */
/*  Module: bitmap_ext4.c                                                     */
/* ========================================================================= */

int  ext4_read_bitmap(int fd, uint64_t part_offset, uint64_t part_size,
                      block_bitmap_t *bm);
void bitmap_free(block_bitmap_t *bm);

/* helper: check if block N is used */
static inline bool bitmap_is_used(const block_bitmap_t *bm, uint64_t block)
{
    if (block >= bm->total_blocks) return false;
    return (bm->bitmap[block / 8] >> (block % 8)) & 1;
}

/* ========================================================================= */
/*  Module: bitmap_xfs.c                                                      */
/* ========================================================================= */

int  xfs_read_bitmap(int fd, uint64_t part_offset, uint64_t part_size,
                     block_bitmap_t *bm);

/* ========================================================================= */
/*  Module: lvm.c — LVM PV/LV awareness                                      */
/* ========================================================================= */

#define MAX_LVS  32

typedef struct {
    char     dm_name[128];        /* e.g. "cs-root" */
    char     dev_path[MAX_PATH_LEN]; /* e.g. "/dev/mapper/cs-root" */
    uint64_t lv_sectors;          /* LV size in 512-byte sectors */
    uint64_t pv_start_sector;     /* start sector on the PV device */
    fs_type_t fs_type;            /* filesystem inside LV */
    char     fs_label[64];
    char     fs_uuid[48];
} lv_info_t;

typedef struct {
    int       num_lvs;
    lv_info_t lvs[MAX_LVS];
} lvm_scan_result_t;

/* Scan for LVs that reside on a given partition (by major:minor).
 * Returns 0 on success. Populates result with found LVs. */
int  lvm_scan_lvs(const char *part_dev_path, lvm_scan_result_t *result);

/* Build a combined bitmap for an LVM partition by reading each LV's
 * filesystem bitmap and mapping it back to partition-relative offsets.
 * The resulting bitmap covers the entire partition in partition block units.
 * part_fd: fd opened on the DISK (not partition), part_offset: partition byte offset.
 * Returns 0 on success. */
int  lvm_build_bitmap(const char *part_dev_path,
                      uint64_t part_offset, uint64_t part_size,
                      block_bitmap_t *bm);

/* ========================================================================= */
/*  Module: vdisk_writer.c — VMDK / VHD / VDI output                         */
/* ========================================================================= */

vdisk_writer_t *vdisk_create(const char *path, vdisk_format_t fmt,
                             uint64_t capacity);
int   vdisk_write(vdisk_writer_t *w, uint64_t offset,
                  const void *buf, size_t len);
int   vdisk_write_zero(vdisk_writer_t *w, uint64_t offset, uint64_t len);
int   vdisk_close(vdisk_writer_t *w);
void  vdisk_destroy(vdisk_writer_t *w);
const char *vdisk_error(const vdisk_writer_t *w);

vdisk_format_t vdisk_format_from_ext(const char *ext);
const char    *vdisk_format_name(vdisk_format_t fmt);

/* ========================================================================= */
/*  Module: imaging.c — orchestrate the disk-to-image process                 */
/* ========================================================================= */

typedef struct {
    disk_info_t    *disk;
    int             disk_fd;
    const char     *output_path;
    vdisk_format_t  format;
    progress_cb_t   progress;
    uint32_t        buf_size;        /* IO buffer size (default 8MB) */
} imaging_config_t;

int  imaging_run(const imaging_config_t *cfg);

/* ========================================================================= */
/*  Module: progress.c                                                        */
/* ========================================================================= */

void progress_print(uint64_t bytes_done, uint64_t bytes_total,
                    uint64_t data_written, double speed_mbps);

/* ========================================================================= */
/*  Utility                                                                   */
/* ========================================================================= */

static inline uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* Format bytes as human-readable string (e.g. "1.23 GB") */
void format_size(uint64_t bytes, char *buf, size_t buflen);

#endif /* D2V_COMMON_H */
