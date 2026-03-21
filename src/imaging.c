/*
 * imaging.c — disk-to-image with producer-consumer pipeline
 *
 * Architecture (same as the Windows version):
 *   Reader thread: reads source disk → pushes buffers to queue
 *   Writer thread (main): pops buffers from queue → writes to vdisk
 *
 * Buffer pool: N pre-allocated buffers rotate between reader and writer.
 * When reader fills one, it goes to the "data" queue; writer consumes it
 * and returns it to the "free" queue.
 */
#include "common.h"
#include <pthread.h>
#include <time.h>

#define DEFAULT_BUF_SIZE   (8 * 1024 * 1024)   /* 8MB per buffer */
#define NUM_BUFFERS        4                     /* buffer pool size */

/* ========================================================================= */
/*  Buffer pool with thread-safe queues                                       */
/* ========================================================================= */

typedef struct {
    uint8_t  *data;
    uint64_t  disk_offset;     /* absolute disk offset for this chunk */
    uint32_t  data_length;     /* bytes of real data (0 = sentinel/EOF) */
    uint64_t  zero_length;     /* bytes of zero to write (sparse skip) */
    bool      is_zero;         /* true = zero_length is valid, no data */
} io_buf_t;

typedef struct {
    io_buf_t  *slots[NUM_BUFFERS + 1]; /* ring buffer (size+1 for full/empty) */
    int        head, tail, capacity;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} buf_queue_t;

static void queue_init(buf_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->capacity = NUM_BUFFERS + 1;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_destroy(buf_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static void queue_push(buf_queue_t *q, io_buf_t *buf)
{
    pthread_mutex_lock(&q->mutex);
    while (((q->tail + 1) % q->capacity) == q->head)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->slots[q->tail] = buf;
    q->tail = (q->tail + 1) % q->capacity;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static io_buf_t *queue_pop(buf_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->head == q->tail)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    io_buf_t *buf = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return buf;
}

/* ========================================================================= */
/*  Reader thread context                                                     */
/* ========================================================================= */

typedef struct {
    int             src_fd;
    disk_info_t    *disk;
    uint32_t        chunk_size;

    buf_queue_t     free_q;     /* writer returns empty buffers here */
    buf_queue_t     data_q;     /* reader pushes filled buffers here */

    int             error;      /* set by reader on failure */
    char            errmsg[256];
} pipeline_ctx_t;

/* ========================================================================= */
/*  Reader: enqueue a data range (reads from disk)                            */
/* ========================================================================= */

static int reader_enqueue_range(pipeline_ctx_t *ctx, uint64_t offset, uint64_t length)
{
    while (length > 0) {
        uint32_t chunk = (uint32_t)min_u64(length, ctx->chunk_size);

        io_buf_t *buf = queue_pop(&ctx->free_q);
        ssize_t rd = pread(ctx->src_fd, buf->data, chunk, offset);
        if (rd <= 0) {
            snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                     "read failed at offset %llu: %s",
                     (unsigned long long)offset, strerror(errno));
            ctx->error = 1;
            /* Return buf to free so writer doesn't hang */
            buf->data_length = 0;
            queue_push(&ctx->free_q, buf);
            return -1;
        }

        buf->disk_offset = offset;
        buf->data_length = (uint32_t)rd;
        buf->zero_length = 0;
        buf->is_zero = false;
        queue_push(&ctx->data_q, buf);

        offset += rd;
        length -= rd;
    }
    return 0;
}

/* ========================================================================= */
/*  Reader: enqueue a zero range (no disk read needed)                        */
/* ========================================================================= */

static int reader_enqueue_zero(pipeline_ctx_t *ctx, uint64_t offset, uint64_t length)
{
    /* Send zero ranges in chunks so writer can update progress */
    while (length > 0) {
        uint64_t chunk = min_u64(length, 256ULL * 1024 * 1024); /* 256MB per zero msg */

        io_buf_t *buf = queue_pop(&ctx->free_q);
        buf->disk_offset = offset;
        buf->data_length = 0;
        buf->zero_length = chunk;
        buf->is_zero = true;
        queue_push(&ctx->data_q, buf);

        offset += chunk;
        length -= chunk;
    }
    return 0;
}

/* ========================================================================= */
/*  Reader: process a partition with bitmap (used-only)                       */
/* ========================================================================= */

static int reader_partition_used_only(pipeline_ctx_t *ctx,
                                      partition_info_t *part,
                                      const block_bitmap_t *bm)
{
    uint64_t part_off = part->offset;
    uint32_t bs = bm->block_size;
    uint64_t blk = 0;

    while (blk < bm->total_blocks) {
        bool used = bitmap_is_used(bm, blk);

        uint64_t run_start = blk;
        while (blk < bm->total_blocks && bitmap_is_used(bm, blk) == used)
            blk++;
        uint64_t run_blocks = blk - run_start;
        uint64_t run_offset = part_off + run_start * bs;
        uint64_t run_bytes  = run_blocks * bs;

        if (run_offset + run_bytes > part_off + part->size)
            run_bytes = part_off + part->size - run_offset;

        if (used) {
            if (reader_enqueue_range(ctx, run_offset, run_bytes) < 0) return -1;
        } else {
            if (reader_enqueue_zero(ctx, run_offset, run_bytes) < 0) return -1;
        }
    }

    uint64_t covered = (uint64_t)bm->total_blocks * bs;
    if (covered < part->size) {
        if (reader_enqueue_range(ctx, part_off + covered, part->size - covered) < 0)
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/*  Reader thread entry point                                                 */
/* ========================================================================= */

static void *reader_thread(void *arg)
{
    pipeline_ctx_t *ctx = (pipeline_ctx_t *)arg;
    disk_info_t *disk = ctx->disk;
    int src_fd = ctx->src_fd;
    uint64_t disk_pos = 0;
    int i;

    for (i = 0; i < disk->num_partitions; i++) {
        partition_info_t *part = &disk->partitions[i];

        /* Gap before partition */
        if (disk_pos < part->offset) {
            if (reader_enqueue_range(ctx, disk_pos, part->offset - disk_pos) < 0)
                goto done;
        }

        if (!part->selected) {
            /* Excluded */
            if (reader_enqueue_zero(ctx, part->offset, part->size) < 0)
                goto done;
        }
        else if (part->copy_mode == 1 &&
                 (part->fs_type == FS_EXT4 || part->fs_type == FS_EXT3 ||
                  part->fs_type == FS_EXT2 || part->fs_type == FS_XFS ||
                  part->fs_type == FS_LVM)) {
            /* Used-only: read bitmap in reader thread (bitmap reading is fast) */
            block_bitmap_t bm;
            int bm_rc = -1;
            if (part->fs_type == FS_LVM)
                bm_rc = lvm_build_bitmap(part->dev_path, part->offset, part->size, &bm);
            else if (part->fs_type == FS_EXT4 || part->fs_type == FS_EXT3 || part->fs_type == FS_EXT2)
                bm_rc = ext4_read_bitmap(src_fd, part->offset, part->size, &bm);
            else if (part->fs_type == FS_XFS)
                bm_rc = xfs_read_bitmap(src_fd, part->offset, part->size, &bm);

            if (bm_rc < 0) {
                snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                         "bitmap read failed for partition #%d (%s)",
                         part->number, part->dev_path);
                ctx->error = 1;
                goto done;
            }
            if (reader_partition_used_only(ctx, part, &bm) < 0) {
                bitmap_free(&bm);
                goto done;
            }
            bitmap_free(&bm);
        }
        else {
            /* Full copy */
            if (reader_enqueue_range(ctx, part->offset, part->size) < 0)
                goto done;
        }

        disk_pos = part->offset + part->size;
    }

    /* Trailing area */
    if (disk_pos < disk->size) {
        if (reader_enqueue_range(ctx, disk_pos, disk->size - disk_pos) < 0)
            goto done;
    }

done:
    /* Send EOF sentinel */
    {
        io_buf_t *eof = queue_pop(&ctx->free_q);
        eof->data_length = 0;
        eof->zero_length = 0;
        eof->is_zero = false;
        queue_push(&ctx->data_q, eof);
    }
    return NULL;
}

/* ========================================================================= */
/*  Public: run imaging with pipeline                                         */
/* ========================================================================= */

int imaging_run(const imaging_config_t *cfg)
{
    disk_info_t *disk = cfg->disk;
    uint32_t buf_size = cfg->buf_size ? cfg->buf_size : DEFAULT_BUF_SIZE;

    /* Print partition plan (before starting threads) */
    char sz[32];
    format_size(disk->size, sz, sizeof(sz));
    fprintf(stderr, "Creating %s image: %s\n", vdisk_format_name(cfg->format), cfg->output_path);
    fprintf(stderr, "Source: %s (%s)\n", disk->dev_path, sz);

    int i;
    for (i = 0; i < disk->num_partitions; i++) {
        partition_info_t *part = &disk->partitions[i];
        if (!part->selected) {
            format_size(part->size, sz, sizeof(sz));
            fprintf(stderr, "  Partition #%d (%s): EXCLUDED — skipping %s\n",
                    part->number, fs_type_name(part->fs_type), sz);
        } else if (part->copy_mode == 1) {
            fprintf(stderr, "  Partition #%d (%s): used-only mode\n",
                    part->number, fs_type_name(part->fs_type));
        } else {
            format_size(part->size, sz, sizeof(sz));
            fprintf(stderr, "  Partition #%d (%s): full copy %s\n",
                    part->number, fs_type_name(part->fs_type), sz);
        }
    }

    /* Create output vdisk */
    vdisk_writer_t *vw = vdisk_create(cfg->output_path, cfg->format, disk->size);
    if (!vw) {
        fprintf(stderr, "Error: failed to create output image\n");
        return -1;
    }

    /* Initialize pipeline */
    pipeline_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.src_fd = cfg->disk_fd;
    ctx.disk = disk;
    ctx.chunk_size = buf_size;
    queue_init(&ctx.free_q);
    queue_init(&ctx.data_q);

    /* Allocate buffer pool */
    io_buf_t buffers[NUM_BUFFERS];
    memset(buffers, 0, sizeof(buffers));
    for (i = 0; i < NUM_BUFFERS; i++) {
        buffers[i].data = (uint8_t *)malloc(buf_size);
        if (!buffers[i].data) {
            fprintf(stderr, "Error: buffer allocation failed\n");
            int j;
            for (j = 0; j < i; j++) free(buffers[j].data);
            vdisk_close(vw); vdisk_destroy(vw);
            queue_destroy(&ctx.free_q); queue_destroy(&ctx.data_q);
            return -1;
        }
        queue_push(&ctx.free_q, &buffers[i]);
    }

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Start reader thread */
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, reader_thread, &ctx);

    /* Writer loop (main thread) */
    uint64_t total_done = 0;
    uint64_t data_written = 0;
    int rc = 0;

    for (;;) {
        io_buf_t *buf = queue_pop(&ctx.data_q);

        /* EOF sentinel: data_length=0, zero_length=0, is_zero=false */
        if (buf->data_length == 0 && buf->zero_length == 0 && !buf->is_zero) {
            queue_push(&ctx.free_q, buf);
            break;
        }

        /* Check reader error */
        if (ctx.error) {
            fprintf(stderr, "\nError: %s\n", ctx.errmsg);
            queue_push(&ctx.free_q, buf);
            rc = -1;
            break;
        }

        if (buf->is_zero) {
            /* Zero range — sparse skip */
            if (vdisk_write_zero(vw, buf->disk_offset, buf->zero_length) < 0) {
                fprintf(stderr, "\nError: write_zero failed: %s\n", vdisk_error(vw));
                queue_push(&ctx.free_q, buf);
                rc = -1;
                break;
            }
            total_done += buf->zero_length;
        } else {
            /* Data range */
            if (vdisk_write(vw, buf->disk_offset, buf->data, buf->data_length) < 0) {
                fprintf(stderr, "\nError: write failed: %s\n", vdisk_error(vw));
                queue_push(&ctx.free_q, buf);
                rc = -1;
                break;
            }
            total_done += buf->data_length;
            data_written += buf->data_length;
        }

        /* Return buffer to free pool */
        queue_push(&ctx.free_q, buf);

        /* Progress */
        if (cfg->progress) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - t_start.tv_sec) + (now.tv_nsec - t_start.tv_nsec) / 1e9;
            double speed = elapsed > 0.1 ? (data_written / elapsed / 1048576.0) : 0;
            cfg->progress(total_done, disk->size, data_written, speed);
        }
    }

    /* Wait for reader to finish */
    pthread_join(reader_tid, NULL);

    /* Check reader error after join */
    if (ctx.error && rc == 0) {
        fprintf(stderr, "\nError: %s\n", ctx.errmsg);
        rc = -1;
    }

    if (vdisk_close(vw) < 0 && rc == 0) rc = -1;
    vdisk_destroy(vw);

    /* Cleanup */
    for (i = 0; i < NUM_BUFFERS; i++)
        free(buffers[i].data);
    queue_destroy(&ctx.free_q);
    queue_destroy(&ctx.data_q);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    fprintf(stderr, "\n");
    if (rc == 0) {
        char dw[32];
        format_size(data_written, dw, sizeof(dw));
        fprintf(stderr, "Done. %s data written in %.1f seconds (%.0f MB/s)\n",
                dw, elapsed, elapsed > 0 ? data_written / elapsed / 1048576.0 : 0);
    } else {
        fprintf(stderr, "Imaging failed.\n");
    }
    return rc;
}
