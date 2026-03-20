/*
 * imaging.c — orchestrate disk-to-image process
 *
 * For each partition:
 *   - If excluded: write zeros (WriteZero → sparse skip)
 *   - If copy_mode == 0 (full space): read all bytes, write to vdisk
 *   - If copy_mode == 1 (used only): read bitmap, only copy used blocks
 * Non-partition areas (MBR, GPT, gaps): always copied
 */
#include "common.h"
#include <time.h>

#define DEFAULT_BUF_SIZE  (8 * 1024 * 1024)   /* 8MB IO buffer */

/* ========================================================================= */
/*  Copy a contiguous range from disk to vdisk                                */
/* ========================================================================= */

static int copy_range(int src_fd, vdisk_writer_t *vw,
                      uint64_t offset, uint64_t length,
                      uint8_t *buf, uint32_t buf_size,
                      uint64_t *total_done, uint64_t total_size,
                      uint64_t *data_written, progress_cb_t progress,
                      struct timespec *t_start)
{
    while (length > 0) {
        uint32_t chunk = (uint32_t)min_u64(length, buf_size);
        ssize_t rd = pread(src_fd, buf, chunk, offset);
        if (rd <= 0) {
            fprintf(stderr, "\nError: read failed at offset %llu: %s\n",
                    (unsigned long long)offset, strerror(errno));
            return -1;
        }

        if (vdisk_write(vw, offset, buf, (size_t)rd) < 0) {
            fprintf(stderr, "\nError: write failed: %s\n", vdisk_error(vw));
            return -1;
        }

        offset += rd;
        length -= rd;
        *total_done += rd;
        *data_written += rd;

        if (progress) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - t_start->tv_sec) + (now.tv_nsec - t_start->tv_nsec) / 1e9;
            double speed = elapsed > 0.1 ? (*data_written / elapsed / 1048576.0) : 0;
            progress(*total_done, total_size, *data_written, speed);
        }
    }
    return 0;
}

/* ========================================================================= */
/*  Write zeros for a range (sparse skip)                                     */
/* ========================================================================= */

static int zero_range(vdisk_writer_t *vw, uint64_t offset, uint64_t length,
                      uint64_t *total_done, uint64_t total_size,
                      progress_cb_t progress, struct timespec *t_start,
                      uint64_t *data_written)
{
    if (vdisk_write_zero(vw, offset, length) < 0) {
        fprintf(stderr, "\nError: write_zero failed: %s\n", vdisk_error(vw));
        return -1;
    }
    *total_done += length;
    if (progress) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t_start->tv_sec) + (now.tv_nsec - t_start->tv_nsec) / 1e9;
        double speed = elapsed > 0.1 ? (*data_written / elapsed / 1048576.0) : 0;
        progress(*total_done, total_size, *data_written, speed);
    }
    return 0;
}

/* ========================================================================= */
/*  Copy partition with bitmap filtering (used-only mode)                     */
/* ========================================================================= */

static int copy_partition_used_only(int src_fd, vdisk_writer_t *vw,
                                    partition_info_t *part,
                                    const block_bitmap_t *bm,
                                    uint8_t *buf, uint32_t buf_size,
                                    uint64_t *total_done, uint64_t total_size,
                                    uint64_t *data_written, progress_cb_t progress,
                                    struct timespec *t_start)
{
    uint64_t part_off = part->offset;
    uint32_t bs = bm->block_size;

    /* Process blocks in runs of consecutive used/free for efficiency */
    uint64_t blk = 0;
    while (blk < bm->total_blocks) {
        bool used = bitmap_is_used(bm, blk);

        /* Find run length of same state */
        uint64_t run_start = blk;
        while (blk < bm->total_blocks && bitmap_is_used(bm, blk) == used)
            blk++;
        uint64_t run_blocks = blk - run_start;
        uint64_t run_offset = part_off + run_start * bs;
        uint64_t run_bytes  = run_blocks * bs;

        /* Clamp to partition size */
        if (run_offset + run_bytes > part_off + part->size)
            run_bytes = part_off + part->size - run_offset;

        if (used) {
            if (copy_range(src_fd, vw, run_offset, run_bytes, buf, buf_size,
                           total_done, total_size, data_written, progress, t_start) < 0)
                return -1;
        } else {
            if (zero_range(vw, run_offset, run_bytes, total_done, total_size,
                           progress, t_start, data_written) < 0)
                return -1;
        }
    }

    /* Handle any remaining bytes beyond bitmap coverage */
    uint64_t covered = (uint64_t)bm->total_blocks * bs;
    if (covered < part->size) {
        uint64_t tail_off = part_off + covered;
        uint64_t tail_len = part->size - covered;
        if (copy_range(src_fd, vw, tail_off, tail_len, buf, buf_size,
                       total_done, total_size, data_written, progress, t_start) < 0)
            return -1;
    }

    return 0;
}

/* ========================================================================= */
/*  Public: run imaging                                                       */
/* ========================================================================= */

int imaging_run(const imaging_config_t *cfg)
{
    disk_info_t *disk = cfg->disk;
    int src_fd = cfg->disk_fd;
    uint32_t buf_size = cfg->buf_size ? cfg->buf_size : DEFAULT_BUF_SIZE;

    /* Allocate IO buffer */
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) {
        fprintf(stderr, "Error: failed to allocate %u byte IO buffer\n", buf_size);
        return -1;
    }

    /* Create output vdisk */
    vdisk_writer_t *vw = vdisk_create(cfg->output_path, cfg->format, disk->size);
    if (!vw) {
        fprintf(stderr, "Error: failed to create output image\n");
        free(buf);
        return -1;
    }

    fprintf(stderr, "Creating %s image: %s\n", vdisk_format_name(cfg->format), cfg->output_path);
    char sz[32];
    format_size(disk->size, sz, sizeof(sz));
    fprintf(stderr, "Source: %s (%s)\n", disk->dev_path, sz);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    uint64_t total_done = 0;
    uint64_t data_written = 0;
    uint64_t disk_pos = 0;
    int rc = 0;

    /* Walk through the disk sequentially, handling partitions and gaps */
    for (int i = 0; i < disk->num_partitions; i++) {
        partition_info_t *part = &disk->partitions[i];

        /* Copy gap before this partition (MBR, GPT header, etc.) */
        if (disk_pos < part->offset) {
            uint64_t gap = part->offset - disk_pos;
            fprintf(stderr, "  Copying gap [%llu - %llu] (%llu bytes)\n",
                    (unsigned long long)disk_pos, (unsigned long long)part->offset,
                    (unsigned long long)gap);
            if ((rc = copy_range(src_fd, vw, disk_pos, gap, buf, buf_size,
                                 &total_done, disk->size, &data_written,
                                 cfg->progress, &t_start)) < 0)
                goto done;
            disk_pos = part->offset;
        }

        if (!part->selected) {
            /* Excluded partition: write zeros */
            fprintf(stderr, "  Partition #%d (%s): EXCLUDED — skipping %s\n",
                    part->number, fs_type_name(part->fs_type),
                    (format_size(part->size, sz, sizeof(sz)), sz));
            if ((rc = zero_range(vw, part->offset, part->size, &total_done,
                                 disk->size, cfg->progress, &t_start, &data_written)) < 0)
                goto done;
        }
        else if (part->copy_mode == 1 &&
                 (part->fs_type == FS_EXT4 || part->fs_type == FS_EXT3 ||
                  part->fs_type == FS_EXT2 || part->fs_type == FS_XFS ||
                  part->fs_type == FS_LVM)) {
            /* Used-only mode with supported filesystem or LVM */
            fprintf(stderr, "  Partition #%d (%s): used-only mode\n",
                    part->number, fs_type_name(part->fs_type));

            block_bitmap_t bm;
            int bm_rc = -1;
            if (part->fs_type == FS_LVM)
                bm_rc = lvm_build_bitmap(part->dev_path, part->offset, part->size, &bm);
            else if (part->fs_type == FS_EXT4 || part->fs_type == FS_EXT3 || part->fs_type == FS_EXT2)
                bm_rc = ext4_read_bitmap(src_fd, part->offset, part->size, &bm);
            else if (part->fs_type == FS_XFS)
                bm_rc = xfs_read_bitmap(src_fd, part->offset, part->size, &bm);

            if (bm_rc < 0) {
                fprintf(stderr, "\nError: bitmap read failed for partition #%d (%s).\n",
                        part->number, part->dev_path);
                fprintf(stderr, "Cannot guarantee data integrity in used-only mode.\n");
                fprintf(stderr, "Aborting. To force full copy, remove --used-only for this partition.\n");
                rc = -1;
                goto done;
            } else {
                rc = copy_partition_used_only(src_fd, vw, part, &bm, buf, buf_size,
                                             &total_done, disk->size, &data_written,
                                             cfg->progress, &t_start);
                bitmap_free(&bm);
            }
            if (rc < 0) goto done;
        }
        else {
            /* Full copy */
            fprintf(stderr, "  Partition #%d (%s): full copy %s\n",
                    part->number, fs_type_name(part->fs_type),
                    (format_size(part->size, sz, sizeof(sz)), sz));
            if ((rc = copy_range(src_fd, vw, part->offset, part->size, buf, buf_size,
                                 &total_done, disk->size, &data_written,
                                 cfg->progress, &t_start)) < 0)
                goto done;
        }

        disk_pos = part->offset + part->size;
    }

    /* Copy trailing area (backup GPT, etc.) */
    if (disk_pos < disk->size) {
        uint64_t tail = disk->size - disk_pos;
        fprintf(stderr, "  Copying tail [%llu - %llu] (%llu bytes)\n",
                (unsigned long long)disk_pos, (unsigned long long)disk->size,
                (unsigned long long)tail);
        if ((rc = copy_range(src_fd, vw, disk_pos, tail, buf, buf_size,
                             &total_done, disk->size, &data_written,
                             cfg->progress, &t_start)) < 0)
            goto done;
    }

done:
    if (vdisk_close(vw) < 0 && rc == 0) rc = -1;
    vdisk_destroy(vw);
    free(buf);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    fprintf(stderr, "\n");
    if (rc == 0) {
        char dw[32];
        format_size(data_written, dw, sizeof(dw));
        fprintf(stderr, "Done. %s data written in %.1f seconds (%.0f MB/s)\n",
                dw, elapsed, data_written / elapsed / 1048576.0);
    } else {
        fprintf(stderr, "Imaging failed.\n");
    }
    return rc;
}
