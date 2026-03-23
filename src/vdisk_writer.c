/*
 * vdisk_writer.c — VMDK / VHD / VDI dynamic image writer for Linux
 *
 * Ported from VDiskWriter.cpp (Windows version).
 * Replaces Win32 API (CreateFile/WriteFile/SetFilePointerEx) with POSIX
 * (open/write/lseek). All format logic is identical.
 */
#include "common.h"
#include <time.h>

/* ========================================================================= */
/*  Internal state                                                            */
/* ========================================================================= */

#define VMDK_SECTOR_SIZE        512
#define VMDK_GRAIN_SIZE_SECTORS 128
#define VMDK_GRAIN_SIZE         (128*512)   /* 65536 = 64KB */
#define VMDK_GT_ENTRIES         512
#define VMDK_GT_SIZE_BYTES      (512*4)     /* 2048 */
#define VMDK_WRITE_BUF_GRAINS  32           /* 32 x 64KB = 2MB per IO */
#define VMDK_WRITE_BUF_SIZE    (VMDK_WRITE_BUF_GRAINS * VMDK_GRAIN_SIZE)

#define VHD_SECTOR_SIZE        512
#define VHD_BLOCK_SIZE         (2*1024*1024)  /* 2MB */
#define VHD_BITMAP_SIZE        512

#define VDI_BLOCK_SIZE         (1*1024*1024)  /* 1MB */
#define VDI_SECTOR_SIZE        512

struct vdisk_writer {
    int         fd;
    uint64_t    capacity;
    uint64_t    file_pos;
    vdisk_format_t fmt;
    char        errmsg[512];

    /* --- VMDK --- */
    uint32_t    vmdk_num_grains;
    uint32_t    vmdk_num_gts;
    uint32_t   *vmdk_gd;
    uint32_t   *vmdk_gt;
    uint64_t    vmdk_rgd_sector;
    uint64_t    vmdk_rgt_sector;
    uint64_t    vmdk_gd_sector;
    uint64_t    vmdk_gt_sector;
    uint64_t    vmdk_data_sector;
    uint64_t    vmdk_next_grain_sector;
    uint8_t    *vmdk_write_buf;
    uint32_t    vmdk_write_buf_used;
    uint8_t    *vmdk_grain_buf;      /* pointer into write_buf */
    uint32_t    vmdk_grain_buf_used;
    uint32_t    vmdk_cur_grain;

    /* --- VHD --- */
    uint32_t    vhd_max_bat;
    uint32_t   *vhd_bat;
    uint64_t    vhd_data_start;
    uint64_t    vhd_next_block;
    uint8_t    *vhd_block_buf;
    uint32_t    vhd_block_buf_used;
    uint32_t    vhd_cur_block;

    /* --- VDI --- */
    uint32_t    vdi_total_blocks;
    uint32_t    vdi_allocated;
    uint32_t   *vdi_block_map;
    uint64_t    vdi_map_offset;
    uint64_t    vdi_data_offset;
    uint64_t    vdi_next_block;
    uint8_t    *vdi_block_buf;
    uint32_t    vdi_block_buf_used;
    uint32_t    vdi_cur_block;
};

#if !defined(__GNUC__) || (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8))
static inline unsigned short __builtin_bswap16(unsigned short x) {
    return (x >> 8) | (x << 8);
}
#endif

/* ========================================================================= */
/*  Helper: zero check                                                        */
/* ========================================================================= */

static bool is_zero_buf(const void *buf, size_t len)
{
    const uint64_t *p = (const uint64_t *)buf;
    size_t n8 = len / 8;
    size_t i;
    for (i = 0; i < n8; i++)
        if (p[i]) return false;
    const uint8_t *tail = (const uint8_t *)buf + n8 * 8;
    for (i = 0; i < (len & 7); i++)
        if (tail[i]) return false;
    return true;
}

/* ========================================================================= */
/*  File IO helpers                                                           */
/* ========================================================================= */

static bool fw(vdisk_writer_t *w, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        ssize_t n = write(w->fd, p, len);
        if (n <= 0) {
            snprintf(w->errmsg, sizeof(w->errmsg), "write failed: %s", strerror(errno));
            return false;
        }
        p += n; len -= (uint32_t)n;
        w->file_pos += n;
    }
    return true;
}

static bool fw_at(vdisk_writer_t *w, uint64_t offset, const void *data, uint32_t len)
{
    if (lseek(w->fd, (off_t)offset, SEEK_SET) == (off_t)-1) {
        snprintf(w->errmsg, sizeof(w->errmsg), "lseek failed: %s", strerror(errno));
        return false;
    }
    w->file_pos = offset;
    return fw(w, data, len);
}

static bool fw_pad(vdisk_writer_t *w, uint64_t target)
{
    if (w->file_pos >= target) return true;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    while (w->file_pos < target) {
        uint32_t chunk = (uint32_t)min_u64(sizeof(zeros), target - w->file_pos);
        if (!fw(w, zeros, chunk)) return false;
    }
    return true;
}

/* ========================================================================= */
/*  Format detection                                                          */
/* ========================================================================= */

vdisk_format_t vdisk_format_from_ext(const char *ext)
{
    if (!ext) return VDISK_FMT_DD;
    if (*ext == '.') ext++;
    if (strcasecmp(ext, "vmdk") == 0) return VDISK_FMT_VMDK;
    if (strcasecmp(ext, "vhd") == 0)  return VDISK_FMT_VHD;
    if (strcasecmp(ext, "vdi") == 0)  return VDISK_FMT_VDI;
    if (strcasecmp(ext, "dd") == 0 || strcasecmp(ext, "raw") == 0 || strcasecmp(ext, "img") == 0)
        return VDISK_FMT_DD;
    return VDISK_FMT_DD;
}

const char *vdisk_format_name(vdisk_format_t fmt)
{
    switch (fmt) {
    case VDISK_FMT_VMDK: return "VMDK";
    case VDISK_FMT_VHD:  return "VHD";
    case VDISK_FMT_VDI:  return "VDI";
    case VDISK_FMT_DD:   return "DD/RAW";
    }
    return "unknown";
}

const char *vdisk_error(const vdisk_writer_t *w) { return w ? w->errmsg : "null writer"; }

/* ========================================================================= */
/*  VMDK create / flush / close                                               */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    uint32_t magicNumber;
    uint32_t version;
    uint32_t flags;
    uint64_t capacity;
    uint64_t grainSize;
    uint64_t descriptorOffset;
    uint64_t descriptorSize;
    uint32_t numGTEsPerGT;
    uint64_t rgdOffset;
    uint64_t gdOffset;
    uint64_t overHead;
    uint8_t  uncleanShutdown;
    char     singleEndLineChar;
    char     nonEndLineChar;
    char     doubleEndLineChar1;
    char     doubleEndLineChar2;
    uint16_t compressAlgorithm;
    uint8_t  pad[433];
} vmdk_header_t;
#pragma pack(pop)

static bool vmdk_flush(vdisk_writer_t *w)
{
    if (w->vmdk_write_buf_used == 0) return true;
    if (!fw(w, w->vmdk_write_buf, w->vmdk_write_buf_used)) return false;
    w->vmdk_next_grain_sector += w->vmdk_write_buf_used / VMDK_SECTOR_SIZE;
    w->vmdk_write_buf_used = 0;
    w->vmdk_grain_buf = w->vmdk_write_buf;
    return true;
}

static int vmdk_create(vdisk_writer_t *w, const char *path, uint64_t cap)
{
    uint64_t cap_sectors = cap / VMDK_SECTOR_SIZE;
    if (cap_sectors % VMDK_GRAIN_SIZE_SECTORS)
        cap_sectors = ((cap_sectors + VMDK_GRAIN_SIZE_SECTORS - 1) / VMDK_GRAIN_SIZE_SECTORS) * VMDK_GRAIN_SIZE_SECTORS;

    w->vmdk_num_grains = (uint32_t)((cap_sectors + VMDK_GRAIN_SIZE_SECTORS - 1) / VMDK_GRAIN_SIZE_SECTORS);
    w->vmdk_num_gts = (w->vmdk_num_grains + VMDK_GT_ENTRIES - 1) / VMDK_GT_ENTRIES;

    w->vmdk_gd = (uint32_t *)calloc(w->vmdk_num_gts, sizeof(uint32_t));
    w->vmdk_gt = (uint32_t *)calloc((size_t)w->vmdk_num_gts * VMDK_GT_ENTRIES, sizeof(uint32_t));
    w->vmdk_write_buf = (uint8_t *)malloc(VMDK_WRITE_BUF_SIZE);
    if (!w->vmdk_gd || !w->vmdk_gt || !w->vmdk_write_buf) return -1;
    w->vmdk_write_buf_used = 0;
    w->vmdk_grain_buf = w->vmdk_write_buf;
    w->vmdk_grain_buf_used = 0;
    w->vmdk_cur_grain = 0;

    /* Layout: Header | Descriptor | RGD+RGTs | GD+GTs | Data */
    uint64_t desc_off = 1, desc_sz = 20;
    uint64_t gd_bytes = (uint64_t)w->vmdk_num_gts * 4;
    uint64_t gd_sec = (gd_bytes + VMDK_SECTOR_SIZE - 1) / VMDK_SECTOR_SIZE;
    uint64_t gt_bytes = (uint64_t)w->vmdk_num_gts * VMDK_GT_SIZE_BYTES;
    uint64_t gt_sec = (gt_bytes + VMDK_SECTOR_SIZE - 1) / VMDK_SECTOR_SIZE;

    uint64_t rgd_start = desc_off + desc_sz;
    uint64_t rgt_start = rgd_start + gd_sec;
    uint64_t gd_start  = rgt_start + gt_sec;
    uint64_t gt_start  = gd_start + gd_sec;
    uint64_t overhead  = gt_start + gt_sec;
    overhead = ((overhead + VMDK_GRAIN_SIZE_SECTORS - 1) / VMDK_GRAIN_SIZE_SECTORS) * VMDK_GRAIN_SIZE_SECTORS;

    w->vmdk_rgd_sector = rgd_start;
    w->vmdk_rgt_sector = rgt_start;
    w->vmdk_gd_sector  = gd_start;
    w->vmdk_gt_sector  = gt_start;
    w->vmdk_data_sector = overhead;
    w->vmdk_next_grain_sector = overhead;

    for (uint32_t i = 0; i < w->vmdk_num_gts; i++)
        w->vmdk_gd[i] = (uint32_t)(gt_start + (uint64_t)i * VMDK_GT_SIZE_BYTES / VMDK_SECTOR_SIZE);

    /* Write header */
    vmdk_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magicNumber = 0x564D444B;
    hdr.version = 1;
    hdr.flags = 3;
    hdr.capacity = cap_sectors;
    hdr.grainSize = VMDK_GRAIN_SIZE_SECTORS;
    hdr.descriptorOffset = desc_off;
    hdr.descriptorSize = desc_sz;
    hdr.numGTEsPerGT = VMDK_GT_ENTRIES;
    hdr.rgdOffset = rgd_start;
    hdr.gdOffset = gd_start;
    hdr.overHead = overhead;
    hdr.singleEndLineChar = '\n';
    hdr.nonEndLineChar = ' ';
    hdr.doubleEndLineChar1 = '\r';
    hdr.doubleEndLineChar2 = '\n';

    if (!fw(w, &hdr, sizeof(hdr))) return -1;

    /* Write descriptor */
    char desc[10240];
    memset(desc, 0, sizeof(desc));
    uint32_t cyl = (uint32_t)(cap_sectors / (16 * 63));
    if (cyl > 16383) cyl = 16383;
    if (cyl == 0) cyl = 1;

    const char *fname = strrchr(path, '/');
    if (fname) fname++; else fname = path;

    uint32_t cid = (uint32_t)(time(NULL) ^ getpid());
    uint32_t u1 = cid ^ 0x578fd28a;

    snprintf(desc, sizeof(desc),
        "# Disk DescriptorFile\n"
        "version=1\n"
        "CID=%08x\n"
        "parentCID=ffffffff\n"
        "createType=\"monolithicSparse\"\n"
        "\n"
        "# Extent description\n"
        "RW %llu SPARSE \"%s\"\n"
        "\n"
        "# The Disk Data Base\n"
        "#DDB\n"
        "\n"
        "ddb.adapterType = \"ide\"\n"
        "ddb.encoding = \"UTF-8\"\n"
        "ddb.geometry.cylinders = \"%u\"\n"
        "ddb.geometry.heads = \"16\"\n"
        "ddb.geometry.sectors = \"63\"\n"
        "ddb.uuid.image = \"%08x-%04x-%04x-%04x-%08x%04x\"\n"
        "ddb.uuid.modification = \"00000000-0000-0000-0000-000000000000\"\n"
        "ddb.uuid.parent = \"00000000-0000-0000-0000-000000000000\"\n"
        "ddb.uuid.parentmodification = \"00000000-0000-0000-0000-000000000000\"\n"
        "ddb.virtualHWVersion = \"4\"\n"
        "ddb.comment = \"disk2vmdk\"\n",
        cid, (unsigned long long)cap_sectors, fname, cyl,
        u1, (uint16_t)(cid>>16), (uint16_t)(cid)|0x4000, (uint16_t)(u1>>16)|0x8000,
        (uint32_t)getpid(), (uint16_t)time(NULL));

    uint32_t desc_pad = (uint32_t)(desc_sz * VMDK_SECTOR_SIZE);
    if (!fw(w, desc, desc_pad)) return -1;

    /* Write placeholders: RGD + RGTs + GD + GTs */
    if (!fw_pad(w, w->file_pos + gd_sec * VMDK_SECTOR_SIZE)) return -1;
    if (!fw_pad(w, w->file_pos + gt_sec * VMDK_SECTOR_SIZE)) return -1;
    if (!fw_pad(w, w->file_pos + gd_sec * VMDK_SECTOR_SIZE)) return -1;
    if (!fw_pad(w, w->file_pos + gt_sec * VMDK_SECTOR_SIZE)) return -1;
    if (!fw_pad(w, overhead * VMDK_SECTOR_SIZE)) return -1;

    return 0;
}

static int vmdk_close(vdisk_writer_t *w)
{
    /* Flush partial grain */
    if (w->vmdk_grain_buf_used > 0) {
        memset(w->vmdk_grain_buf + w->vmdk_grain_buf_used, 0, VMDK_GRAIN_SIZE - w->vmdk_grain_buf_used);
        if (!is_zero_buf(w->vmdk_grain_buf, VMDK_GRAIN_SIZE)) {
            w->vmdk_gt[w->vmdk_cur_grain] = (uint32_t)(w->vmdk_next_grain_sector +
                w->vmdk_write_buf_used / VMDK_SECTOR_SIZE);
            w->vmdk_write_buf_used += VMDK_GRAIN_SIZE;
        }
        w->vmdk_grain_buf_used = 0;
    }
    if (w->vmdk_write_buf_used > 0)
        if (!vmdk_flush(w)) return -1;

    /* Write redundant GD + GTs */
    uint32_t *rgd = (uint32_t *)calloc(w->vmdk_num_gts, sizeof(uint32_t));
    if (!rgd) return -1;
    uint32_t i;
    for (i = 0; i < w->vmdk_num_gts; i++)
        rgd[i] = (uint32_t)(w->vmdk_rgt_sector + (uint64_t)i * VMDK_GT_SIZE_BYTES / VMDK_SECTOR_SIZE);

    if (!fw_at(w, w->vmdk_rgd_sector * VMDK_SECTOR_SIZE, rgd, w->vmdk_num_gts * 4))
    { free(rgd); return -1; }
    for (i = 0; i < w->vmdk_num_gts; i++) {
        if (!fw_at(w, (uint64_t)rgd[i] * VMDK_SECTOR_SIZE,
                   w->vmdk_gt + (size_t)i * VMDK_GT_ENTRIES, VMDK_GT_SIZE_BYTES))
        { free(rgd); return -1; }
    }
    free(rgd);

    /* Write primary GD + GTs */
    if (!fw_at(w, w->vmdk_gd_sector * VMDK_SECTOR_SIZE, w->vmdk_gd, w->vmdk_num_gts * 4))
        return -1;
    for (i = 0; i < w->vmdk_num_gts; i++) {
        if (!fw_at(w, (uint64_t)w->vmdk_gd[i] * VMDK_SECTOR_SIZE,
                   w->vmdk_gt + (size_t)i * VMDK_GT_ENTRIES, VMDK_GT_SIZE_BYTES))
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/*  VHD create / write block / close                                          */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    char     cookie[8];
    uint32_t features;
    uint32_t fileFormatVersion;
    uint64_t dataOffset;
    uint32_t timeStamp;
    char     creatorApp[4];
    uint32_t creatorVersion;
    uint32_t creatorHostOS;
    uint64_t originalSize;
    uint64_t currentSize;
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectorsPerTrack;
    uint32_t diskType;
    uint32_t checksum;
    uint8_t  uniqueId[16];
    uint8_t  savedState;
    uint8_t  reserved[427];
} vhd_footer_t;

typedef struct {
    char     cookie[8];
    uint64_t dataOffset;
    uint64_t tableOffset;
    uint32_t headerVersion;
    uint32_t maxTableEntries;
    uint32_t blockSize;
    uint32_t checksum;
    uint8_t  parentUniqueId[16];
    uint32_t parentTimeStamp;
    uint32_t reserved1;
    uint8_t  parentUnicodeName[512];
    uint8_t  parentLocator[192];
    uint8_t  reserved2[256];
} vhd_dyn_header_t;
#pragma pack(pop)

static uint64_t bswap64(uint64_t v) {
    return __builtin_bswap64(v);
}
static uint32_t bswap32(uint32_t v) {
    return __builtin_bswap32(v);
}
static uint16_t bswap16(uint16_t v) {
    return __builtin_bswap16(v);
}

static uint32_t vhd_checksum(const uint8_t *buf, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += buf[i];
    return ~sum;
}

static int vhd_create(vdisk_writer_t *w, const char *path, uint64_t cap)
{
    w->vhd_max_bat = (uint32_t)((cap + VHD_BLOCK_SIZE - 1) / VHD_BLOCK_SIZE);
    w->vhd_bat = (uint32_t *)malloc((size_t)w->vhd_max_bat * 4);
    w->vhd_block_buf = (uint8_t *)malloc(VHD_BLOCK_SIZE);
    if (!w->vhd_bat || !w->vhd_block_buf) return -1;
    memset(w->vhd_bat, 0xFF, (size_t)w->vhd_max_bat * 4);
    w->vhd_block_buf_used = 0;
    w->vhd_cur_block = 0;

    uint64_t bat_offset = 512 + 1024;
    uint64_t bat_size = (uint64_t)w->vhd_max_bat * 4;
    uint64_t bat_padded = ((bat_size + VHD_SECTOR_SIZE - 1) / VHD_SECTOR_SIZE) * VHD_SECTOR_SIZE;
    w->vhd_data_start = bat_offset + bat_padded;
    w->vhd_next_block = w->vhd_data_start;

    /* Footer */
    vhd_footer_t ft;
    memset(&ft, 0, sizeof(ft));
    memcpy(ft.cookie, "conectix", 8);
    ft.features = bswap32(2);
    ft.fileFormatVersion = bswap32(0x00010000);
    ft.dataOffset = bswap64(512);
    memcpy(ft.creatorApp, "d2v ", 4);
    ft.creatorVersion = bswap32(0x00010000);
    ft.creatorHostOS = bswap32(0x5769326B);
    ft.originalSize = bswap64(cap);
    ft.currentSize = bswap64(cap);

    uint64_t ts = cap / VHD_SECTOR_SIZE;
    if (ts > 65535ULL * 16 * 255) ts = 65535ULL * 16 * 255;
    ft.sectorsPerTrack = 63; ft.heads = 16;
    ft.cylinders = bswap16((uint16_t)(ts / (16 * 63)));
    ft.diskType = bswap32(3);

    uint32_t uid = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    memcpy(ft.uniqueId, &uid, 4);
    uid ^= 0xDEADBEEF;
    memcpy(ft.uniqueId + 4, &uid, 4);

    ft.checksum = 0;
    ft.checksum = bswap32(vhd_checksum((uint8_t *)&ft, sizeof(ft)));

    if (!fw(w, &ft, sizeof(ft))) return -1;

    /* Dynamic header */
    vhd_dyn_header_t dh;
    memset(&dh, 0, sizeof(dh));
    memcpy(dh.cookie, "cxsparse", 8);
    dh.dataOffset = bswap64(0xFFFFFFFFFFFFFFFFULL);
    dh.tableOffset = bswap64(bat_offset);
    dh.headerVersion = bswap32(0x00010000);
    dh.maxTableEntries = bswap32(w->vhd_max_bat);
    dh.blockSize = bswap32(VHD_BLOCK_SIZE);
    dh.checksum = 0;
    dh.checksum = bswap32(vhd_checksum((uint8_t *)&dh, sizeof(dh)));

    if (!fw(w, &dh, sizeof(dh))) return -1;

    /* BAT placeholder */
    if (!fw(w, w->vhd_bat, (uint32_t)bat_size)) return -1;
    if (bat_size < bat_padded)
        if (!fw_pad(w, bat_offset + bat_padded)) return -1;

    return 0;
}

static bool vhd_write_block(vdisk_writer_t *w, uint32_t idx, const void *data, uint32_t len)
{
    uint32_t sec_off = (uint32_t)(w->vhd_next_block / VHD_SECTOR_SIZE);
    w->vhd_bat[idx] = bswap32(sec_off);

    if (lseek(w->fd, (off_t)w->vhd_next_block, SEEK_SET) == (off_t)-1) return false;
    w->file_pos = w->vhd_next_block;

    uint8_t bitmap[VHD_BITMAP_SIZE];
    memset(bitmap, 0xFF, VHD_BITMAP_SIZE);
    if (!fw(w, bitmap, VHD_BITMAP_SIZE)) return false;
    if (!fw(w, data, len)) return false;

    w->vhd_next_block = w->file_pos;
    return true;
}

static int vhd_close(vdisk_writer_t *w)
{
    if (w->vhd_block_buf_used > 0) {
        memset(w->vhd_block_buf + w->vhd_block_buf_used, 0, VHD_BLOCK_SIZE - w->vhd_block_buf_used);
        if (!is_zero_buf(w->vhd_block_buf, VHD_BLOCK_SIZE))
            if (!vhd_write_block(w, w->vhd_cur_block, w->vhd_block_buf, VHD_BLOCK_SIZE)) return -1;
        w->vhd_block_buf_used = 0;
    }

    /* Rewrite BAT */
    if (!fw_at(w, 512 + 1024, w->vhd_bat, w->vhd_max_bat * 4)) return -1;

    /* Read footer from offset 0 and append at end */
    vhd_footer_t ft;
    if (lseek(w->fd, 0, SEEK_SET) == (off_t)-1) return -1;
    if (read(w->fd, &ft, sizeof(ft)) != sizeof(ft)) return -1;

    if (lseek(w->fd, (off_t)w->vhd_next_block, SEEK_SET) == (off_t)-1) return -1;
    w->file_pos = w->vhd_next_block;
    if (!fw(w, &ft, sizeof(ft))) return -1;

    return 0;
}

/* ========================================================================= */
/*  VDI create / write block / close                                          */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct {
    char     szFileInfo[64];
    uint32_t u32Signature;
    uint32_t u32Version;
    uint32_t cbHeader;
    uint32_t u32Type;
    uint32_t fFlags;
    char     szComment[256];
    uint32_t offBlocks;
    uint32_t offData;
    uint32_t u32CylGeometry;
    uint32_t u32HeadGeometry;
    uint32_t u32SectGeometry;
    uint32_t cbSector;
    uint32_t u32Dummy;
    uint64_t cbDisk;
    uint32_t cbBlock;
    uint32_t cbBlockExtra;
    uint32_t cBlocks;
    uint32_t cBlocksAllocated;
    uint8_t  uuidCreate[16];
    uint8_t  uuidModify[16];
    uint8_t  uuidLinkage[16];
    uint8_t  uuidParentModify[16];
    uint8_t  padding[56];
} vdi_header_t;
#pragma pack(pop)

static int vdi_create(vdisk_writer_t *w, const char *path, uint64_t cap)
{
    w->vdi_total_blocks = (uint32_t)((cap + VDI_BLOCK_SIZE - 1) / VDI_BLOCK_SIZE);
    w->vdi_allocated = 0;
    w->vdi_block_map = (uint32_t *)malloc((size_t)w->vdi_total_blocks * 4);
    w->vdi_block_buf = (uint8_t *)malloc(VDI_BLOCK_SIZE);
    if (!w->vdi_block_map || !w->vdi_block_buf) return -1;
    memset(w->vdi_block_map, 0xFF, (size_t)w->vdi_total_blocks * 4);
    w->vdi_block_buf_used = 0;
    w->vdi_cur_block = 0;

    uint32_t hdr_total = 512;
    w->vdi_map_offset = hdr_total;
    uint64_t map_size = (uint64_t)w->vdi_total_blocks * 4;
    uint64_t map_padded = ((map_size + VDI_SECTOR_SIZE - 1) / VDI_SECTOR_SIZE) * VDI_SECTOR_SIZE;
    w->vdi_data_offset = w->vdi_map_offset + map_padded;
    w->vdi_next_block = w->vdi_data_offset;

    vdi_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.szFileInfo, "<<< Oracle VM VirtualBox Disk Image >>>\n", sizeof(hdr.szFileInfo));
    hdr.u32Signature = 0xBEDA107F;
    hdr.u32Version = 0x00010001;
    hdr.cbHeader = 400;
    hdr.u32Type = 1;
    strncpy(hdr.szComment, "Created by disk2vmdk", sizeof(hdr.szComment));
    hdr.offBlocks = (uint32_t)w->vdi_map_offset;
    hdr.offData = (uint32_t)w->vdi_data_offset;
    uint64_t ts = cap / 512;
    hdr.u32CylGeometry = (uint32_t)(ts / (16 * 63));
    if (hdr.u32CylGeometry > 16383) hdr.u32CylGeometry = 16383;
    if (hdr.u32CylGeometry == 0) hdr.u32CylGeometry = 1;
    hdr.u32HeadGeometry = 16;
    hdr.u32SectGeometry = 63;
    hdr.cbSector = 512;
    hdr.cbDisk = cap;
    hdr.cbBlock = VDI_BLOCK_SIZE;
    hdr.cBlocks = w->vdi_total_blocks;
    hdr.cBlocksAllocated = 0;

    uint32_t uid = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    memcpy(hdr.uuidCreate, &uid, 4);
    memcpy(hdr.uuidModify, &uid, 4);

    uint32_t write_sz = sizeof(hdr) < hdr_total ? (uint32_t)sizeof(hdr) : hdr_total;
    if (!fw(w, &hdr, write_sz)) return -1;
    if (sizeof(hdr) < hdr_total) if (!fw_pad(w, hdr_total)) return -1;

    if (!fw(w, w->vdi_block_map, (uint32_t)map_size)) return -1;
    if (map_size < map_padded) if (!fw_pad(w, w->vdi_map_offset + map_padded)) return -1;

    return 0;
}

static bool vdi_write_block(vdisk_writer_t *w, uint32_t idx, const void *data, uint32_t len)
{
    w->vdi_block_map[idx] = w->vdi_allocated++;
    if (lseek(w->fd, (off_t)w->vdi_next_block, SEEK_SET) == (off_t)-1) return false;
    w->file_pos = w->vdi_next_block;
    if (!fw(w, data, len)) return false;
    w->vdi_next_block = w->file_pos;
    return true;
}

static int vdi_close(vdisk_writer_t *w)
{
    if (w->vdi_block_buf_used > 0) {
        memset(w->vdi_block_buf + w->vdi_block_buf_used, 0, VDI_BLOCK_SIZE - w->vdi_block_buf_used);
        if (!is_zero_buf(w->vdi_block_buf, VDI_BLOCK_SIZE))
            if (!vdi_write_block(w, w->vdi_cur_block, w->vdi_block_buf, VDI_BLOCK_SIZE)) return -1;
        w->vdi_block_buf_used = 0;
    }
    if (!fw_at(w, w->vdi_map_offset, w->vdi_block_map, w->vdi_total_blocks * 4)) return -1;

    /* Update cBlocksAllocated in header */
    /* offset of cBlocksAllocated = offsetof(vdi_header_t, cBlocksAllocated) */
    uint32_t off_alloc = (uint32_t)((char *)&((vdi_header_t *)0)->cBlocksAllocated - (char *)0);
    if (!fw_at(w, off_alloc, &w->vdi_allocated, 4)) return -1;

    return 0;
}

/* ========================================================================= */
/*  Public API                                                                */
/* ========================================================================= */

vdisk_writer_t *vdisk_create(const char *path, vdisk_format_t fmt, uint64_t capacity)
{
    vdisk_writer_t *w = (vdisk_writer_t *)calloc(1, sizeof(vdisk_writer_t));
    if (!w) return NULL;

    w->capacity = capacity;
    w->fmt = fmt;
    w->fd = -1;

    bool use_stdout = (strcmp(path, "-") == 0 || strcmp(path, "/dev/stdout") == 0);

    if (use_stdout) {
        if (fmt != VDISK_FMT_DD) {
            snprintf(w->errmsg, sizeof(w->errmsg),
                "stdout output only supports DD/RAW format (VMDK/VHD/VDI require seek)");
            free(w);
            return NULL;
        }
        w->fd = STDOUT_FILENO;
    }
    else if (fmt == VDISK_FMT_DD) {
        w->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
    } else {
        w->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
    }
    if (w->fd < 0) {
        snprintf(w->errmsg, sizeof(w->errmsg), "cannot create %s: %s", path, strerror(errno));
        free(w);
        return NULL;
    }

    int rc = 0;
    switch (fmt) {
    case VDISK_FMT_VMDK: rc = vmdk_create(w, path, capacity); break;
    case VDISK_FMT_VHD:  rc = vhd_create(w, path, capacity);  break;
    case VDISK_FMT_VDI:  rc = vdi_create(w, path, capacity);  break;
    case VDISK_FMT_DD:   break; /* nothing to initialize */
    }

    if (rc < 0) {
        if (!w->errmsg[0]) snprintf(w->errmsg, sizeof(w->errmsg), "format init failed");
        close(w->fd);
        unlink(path);
        free(w);
        return NULL;
    }

    return w;
}

int vdisk_write(vdisk_writer_t *w, uint64_t offset, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;

    if (w->fmt == VDISK_FMT_DD) {
        if (lseek(w->fd, (off_t)offset, SEEK_SET) == (off_t)-1) return -1;
        w->file_pos = offset;
        return fw(w, src, (uint32_t)len) ? 0 : -1;
    }

    while (len > 0) {
        switch (w->fmt) {
        case VDISK_FMT_VMDK: {
            uint32_t grain = (uint32_t)(offset / VMDK_GRAIN_SIZE);
            uint32_t off_in = (uint32_t)(offset % VMDK_GRAIN_SIZE);
            uint32_t to_write = VMDK_GRAIN_SIZE - off_in;
            if (to_write > len) to_write = (uint32_t)len;

            if (grain != w->vmdk_cur_grain || w->vmdk_grain_buf_used == 0) {
                if (w->vmdk_grain_buf_used > 0 && w->vmdk_cur_grain != grain) {
                    if (w->vmdk_grain_buf_used < VMDK_GRAIN_SIZE)
                        memset(w->vmdk_grain_buf + w->vmdk_grain_buf_used, 0, VMDK_GRAIN_SIZE - w->vmdk_grain_buf_used);
                    if (is_zero_buf(w->vmdk_grain_buf, VMDK_GRAIN_SIZE)) {
                        if (w->vmdk_write_buf_used > 0) if (!vmdk_flush(w)) return -1;
                    } else {
                        w->vmdk_gt[w->vmdk_cur_grain] = (uint32_t)(w->vmdk_next_grain_sector +
                            w->vmdk_write_buf_used / VMDK_SECTOR_SIZE);
                        w->vmdk_write_buf_used += VMDK_GRAIN_SIZE;
                        if (w->vmdk_write_buf_used >= VMDK_WRITE_BUF_SIZE) if (!vmdk_flush(w)) return -1;
                    }
                    w->vmdk_grain_buf_used = 0;
                }
                w->vmdk_cur_grain = grain;
                w->vmdk_grain_buf = w->vmdk_write_buf + w->vmdk_write_buf_used;
                if (w->vmdk_grain_buf_used == 0 && off_in > 0)
                    memset(w->vmdk_grain_buf, 0, off_in);
                w->vmdk_grain_buf_used = off_in;
            }

            memcpy(w->vmdk_grain_buf + off_in, src, to_write);
            if (off_in + to_write > w->vmdk_grain_buf_used)
                w->vmdk_grain_buf_used = off_in + to_write;

            if (w->vmdk_grain_buf_used == VMDK_GRAIN_SIZE) {
                if (is_zero_buf(w->vmdk_grain_buf, VMDK_GRAIN_SIZE)) {
                    if (w->vmdk_write_buf_used > 0) if (!vmdk_flush(w)) return -1;
                } else {
                    w->vmdk_gt[w->vmdk_cur_grain] = (uint32_t)(w->vmdk_next_grain_sector +
                        w->vmdk_write_buf_used / VMDK_SECTOR_SIZE);
                    w->vmdk_write_buf_used += VMDK_GRAIN_SIZE;
                    if (w->vmdk_write_buf_used >= VMDK_WRITE_BUF_SIZE) if (!vmdk_flush(w)) return -1;
                }
                w->vmdk_grain_buf_used = 0;
            }

            src += to_write; offset += to_write; len -= to_write;
            break;
        }

        case VDISK_FMT_VHD: {
            uint32_t blk = (uint32_t)(offset / VHD_BLOCK_SIZE);
            uint32_t off_in = (uint32_t)(offset % VHD_BLOCK_SIZE);
            uint32_t to_write = VHD_BLOCK_SIZE - off_in;
            if (to_write > len) to_write = (uint32_t)len;

            if (blk != w->vhd_cur_block || w->vhd_block_buf_used == 0) {
                if (w->vhd_block_buf_used > 0 && w->vhd_cur_block != blk) {
                    if (w->vhd_block_buf_used < VHD_BLOCK_SIZE)
                        memset(w->vhd_block_buf + w->vhd_block_buf_used, 0, VHD_BLOCK_SIZE - w->vhd_block_buf_used);
                    if (!is_zero_buf(w->vhd_block_buf, VHD_BLOCK_SIZE))
                        if (!vhd_write_block(w, w->vhd_cur_block, w->vhd_block_buf, VHD_BLOCK_SIZE)) return -1;
                    w->vhd_block_buf_used = 0;
                }
                w->vhd_cur_block = blk;
                if (w->vhd_block_buf_used == 0 && off_in > 0)
                    memset(w->vhd_block_buf, 0, off_in);
                w->vhd_block_buf_used = off_in;
            }

            memcpy(w->vhd_block_buf + off_in, src, to_write);
            if (off_in + to_write > w->vhd_block_buf_used)
                w->vhd_block_buf_used = off_in + to_write;
            if (w->vhd_block_buf_used == VHD_BLOCK_SIZE) {
                if (!is_zero_buf(w->vhd_block_buf, VHD_BLOCK_SIZE))
                    if (!vhd_write_block(w, w->vhd_cur_block, w->vhd_block_buf, VHD_BLOCK_SIZE)) return -1;
                w->vhd_block_buf_used = 0;
            }

            src += to_write; offset += to_write; len -= to_write;
            break;
        }

        case VDISK_FMT_VDI: {
            uint32_t blk = (uint32_t)(offset / VDI_BLOCK_SIZE);
            uint32_t off_in = (uint32_t)(offset % VDI_BLOCK_SIZE);
            uint32_t to_write = VDI_BLOCK_SIZE - off_in;
            if (to_write > len) to_write = (uint32_t)len;

            if (blk != w->vdi_cur_block || w->vdi_block_buf_used == 0) {
                if (w->vdi_block_buf_used > 0 && w->vdi_cur_block != blk) {
                    if (w->vdi_block_buf_used < VDI_BLOCK_SIZE)
                        memset(w->vdi_block_buf + w->vdi_block_buf_used, 0, VDI_BLOCK_SIZE - w->vdi_block_buf_used);
                    if (!is_zero_buf(w->vdi_block_buf, VDI_BLOCK_SIZE))
                        if (!vdi_write_block(w, w->vdi_cur_block, w->vdi_block_buf, VDI_BLOCK_SIZE)) return -1;
                    w->vdi_block_buf_used = 0;
                }
                w->vdi_cur_block = blk;
                if (w->vdi_block_buf_used == 0 && off_in > 0)
                    memset(w->vdi_block_buf, 0, off_in);
                w->vdi_block_buf_used = off_in;
            }

            memcpy(w->vdi_block_buf + off_in, src, to_write);
            if (off_in + to_write > w->vdi_block_buf_used)
                w->vdi_block_buf_used = off_in + to_write;
            if (w->vdi_block_buf_used == VDI_BLOCK_SIZE) {
                if (!is_zero_buf(w->vdi_block_buf, VDI_BLOCK_SIZE))
                    if (!vdi_write_block(w, w->vdi_cur_block, w->vdi_block_buf, VDI_BLOCK_SIZE)) return -1;
                w->vdi_block_buf_used = 0;
            }

            src += to_write; offset += to_write; len -= to_write;
            break;
        }

        default: return -1;
        }
    }
    return 0;
}

int vdisk_write_zero(vdisk_writer_t *w, uint64_t offset, uint64_t len)
{
    uint32_t unit;
    switch (w->fmt) {
    case VDISK_FMT_VMDK: unit = VMDK_GRAIN_SIZE; break;
    case VDISK_FMT_VHD:  unit = VHD_BLOCK_SIZE;  break;
    case VDISK_FMT_VDI:  unit = VDI_BLOCK_SIZE;  break;
    default:             unit = VHD_BLOCK_SIZE;  break;
    }

    /* Phase 1: unaligned head */
    uint32_t off_in_unit = (uint32_t)(offset % unit);
    if (off_in_unit > 0 && len > 0) {
        uint64_t head = min_u64(len, unit - off_in_unit);
        uint8_t zeros[65536];
        memset(zeros, 0, sizeof(zeros));
        while (head > 0) {
            size_t chunk = (size_t)min_u64(head, sizeof(zeros));
            if (vdisk_write(w, offset, zeros, chunk) < 0) return -1;
            offset += chunk; len -= chunk; head -= chunk;
        }
    }

    /* Phase 2: skip whole units */
    uint64_t whole = len / unit;
    if (whole > 0) {
        /* Flush pending buffer */
        switch (w->fmt) {
        case VDISK_FMT_VMDK:
            if (w->vmdk_grain_buf_used > 0) {
                memset(w->vmdk_grain_buf + w->vmdk_grain_buf_used, 0, VMDK_GRAIN_SIZE - w->vmdk_grain_buf_used);
                if (!is_zero_buf(w->vmdk_grain_buf, VMDK_GRAIN_SIZE)) {
                    w->vmdk_gt[w->vmdk_cur_grain] = (uint32_t)(w->vmdk_next_grain_sector +
                        w->vmdk_write_buf_used / VMDK_SECTOR_SIZE);
                    w->vmdk_write_buf_used += VMDK_GRAIN_SIZE;
                }
                w->vmdk_grain_buf_used = 0;
            }
            if (w->vmdk_write_buf_used > 0) if (!vmdk_flush(w)) return -1;
            break;
        case VDISK_FMT_VHD:
            if (w->vhd_block_buf_used > 0) {
                memset(w->vhd_block_buf + w->vhd_block_buf_used, 0, VHD_BLOCK_SIZE - w->vhd_block_buf_used);
                if (!is_zero_buf(w->vhd_block_buf, VHD_BLOCK_SIZE))
                    if (!vhd_write_block(w, w->vhd_cur_block, w->vhd_block_buf, VHD_BLOCK_SIZE)) return -1;
                w->vhd_block_buf_used = 0;
            }
            break;
        case VDISK_FMT_VDI:
            if (w->vdi_block_buf_used > 0) {
                memset(w->vdi_block_buf + w->vdi_block_buf_used, 0, VDI_BLOCK_SIZE - w->vdi_block_buf_used);
                if (!is_zero_buf(w->vdi_block_buf, VDI_BLOCK_SIZE))
                    if (!vdi_write_block(w, w->vdi_cur_block, w->vdi_block_buf, VDI_BLOCK_SIZE)) return -1;
                w->vdi_block_buf_used = 0;
            }
            break;
        default: break;
        }
        uint64_t skip = whole * unit;
        offset += skip; len -= skip;
    }

    /* Phase 3: unaligned tail */
    if (len > 0) {
        uint8_t zeros[65536];
        memset(zeros, 0, sizeof(zeros));
        while (len > 0) {
            size_t chunk = (size_t)min_u64(len, sizeof(zeros));
            if (vdisk_write(w, offset, zeros, chunk) < 0) return -1;
            offset += chunk; len -= chunk;
        }
    }

    return 0;
}

int vdisk_close(vdisk_writer_t *w)
{
    if (!w || w->fd < 0) return 0;
    int rc = 0;
    switch (w->fmt) {
    case VDISK_FMT_VMDK: rc = vmdk_close(w); break;
    case VDISK_FMT_VHD:  rc = vhd_close(w);  break;
    case VDISK_FMT_VDI:  rc = vdi_close(w);  break;
    case VDISK_FMT_DD:   break;
    }
    if (w->fd > STDERR_FILENO)  /* don't close stdin/stdout/stderr */
        close(w->fd);
    w->fd = -1;
    return rc;
}

void vdisk_destroy(vdisk_writer_t *w)
{
    if (!w) return;
    if (w->fd > STDERR_FILENO) close(w->fd);
    free(w->vmdk_gd); free(w->vmdk_gt); free(w->vmdk_write_buf);
    free(w->vhd_bat); free(w->vhd_block_buf);
    free(w->vdi_block_map); free(w->vdi_block_buf);
    free(w);
}
