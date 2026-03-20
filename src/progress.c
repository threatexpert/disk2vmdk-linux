/*
 * progress.c — terminal progress display (throttled to 1 update/sec)
 */
#include "common.h"
#include <time.h>

void progress_print(uint64_t bytes_done, uint64_t bytes_total,
                    uint64_t data_written, double speed_mbps)
{
    static time_t last_time = 0;
    time_t now = time(NULL);

    /* Only update once per second, unless this is the final call (100%) */
    int pct = bytes_total > 0 ? (int)(bytes_done * 100 / bytes_total) : 0;
    if (pct > 100) pct = 100;
    if (now == last_time && pct < 100)
        return;
    last_time = now;

    char done_str[32], total_str[32], data_str[32];
    format_size(bytes_done, done_str, sizeof(done_str));
    format_size(bytes_total, total_str, sizeof(total_str));
    format_size(data_written, data_str, sizeof(data_str));

    int bar_width = 30;
    int filled = bar_width * pct / 100;

    fprintf(stderr, "\r  [");
    for (int i = 0; i < bar_width; i++)
        fputc(i < filled ? '=' : ' ', stderr);
    fprintf(stderr, "] %3d%%  %s/%s  data:%s  %.0f MB/s  ",
            pct, done_str, total_str, data_str, speed_mbps);
    fflush(stderr);
}
