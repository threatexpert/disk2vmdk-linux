/*
 * disk.c — disk enumeration and raw IO
 */
#include "common.h"
#include <dirent.h>
#include <ctype.h>

void format_size(uint64_t bytes, char *buf, size_t buflen)
{
    const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double val = (double)bytes;
    int u = 0;
    while (val >= 1000.0 && u < 5) { val /= 1000.0; u++; }
    if (u == 0)
        snprintf(buf, buflen, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, buflen, "%.2f %s", val, units[u]);
}

/* Check if a block device name looks like a whole disk (not a partition) */
static bool is_whole_disk(const char *name)
{
    size_t len = strlen(name);
    if (len == 0) return false;

    /* sd[a-z], hd[a-z], vd[a-z] — whole disk if last char is alpha */
    if ((strncmp(name, "sd", 2) == 0 ||
         strncmp(name, "hd", 2) == 0 ||
         strncmp(name, "vd", 2) == 0) && isalpha(name[len-1]))
        return true;

    /* nvmeXnY — whole disk if no 'p' partition suffix */
    if (strncmp(name, "nvme", 4) == 0 && !strchr(name + 4, 'p'))
        return true;

    /* mmcblkX */
    if (strncmp(name, "mmcblk", 6) == 0 && !strchr(name + 6, 'p'))
        return true;

    return false;
}

/* Read a sysfs attribute as string */
static int read_sysfs_str(const char *path, char *buf, size_t buflen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { buf[0] = '\0'; return -1; }
    ssize_t n = read(fd, buf, buflen - 1);
    close(fd);
    if (n <= 0) { buf[0] = '\0'; return -1; }
    buf[n] = '\0';
    /* strip trailing newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return 0;
}

int disk_open(const char *dev_path, disk_info_t *info)
{
    memset(info, 0, sizeof(*info));
    strncpy(info->dev_path, dev_path, sizeof(info->dev_path) - 1);

    int fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }

    /* Get disk size */
    if (ioctl(fd, BLKGETSIZE64, &info->size) < 0) {
        fprintf(stderr, "Error: cannot get size of %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Get sector size */
    int ssz = 0;
    if (ioctl(fd, BLKSSZGET, &ssz) == 0 && ssz > 0)
        info->sector_size = (uint32_t)ssz;
    else
        info->sector_size = 512;

    /* Try to get model from sysfs */
    const char *basename = strrchr(dev_path, '/');
    if (basename) basename++; else basename = dev_path;
    char syspath[512];
    snprintf(syspath, sizeof(syspath), "/sys/block/%s/device/model", basename);
    read_sysfs_str(syspath, info->model, sizeof(info->model));
    if (!info->model[0]) {
        snprintf(syspath, sizeof(syspath), "/sys/block/%s/device/vendor", basename);
        read_sysfs_str(syspath, info->model, sizeof(info->model));
    }

    return fd;
}

void disk_close(int fd)
{
    if (fd >= 0) close(fd);
}

int disk_list_all(void)
{
    DIR *d = opendir("/sys/block");
    if (!d) {
        fprintf(stderr, "Error: cannot read /sys/block: %s\n", strerror(errno));
        return -1;
    }

    printf("%-12s %-36s %12s  %s\n", "Device", "Model", "Size", "Partitions");
    printf("%-12s %-36s %12s  %s\n", "------", "-----", "----", "----------");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!is_whole_disk(ent->d_name)) continue;

        char devpath[256];
        snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);

        disk_info_t info;
        int fd = disk_open(devpath, &info);
        if (fd < 0) continue;

        /* Scan partitions */
        partition_scan(fd, &info);
        disk_close(fd);

        char sz[32];
        format_size(info.size, sz, sizeof(sz));

        printf("%-12s %-36s %12s  ", ent->d_name, info.model[0] ? info.model : "-", sz);

        for (int i = 0; i < info.num_partitions; i++) {
            if (i > 0) printf(", ");
            char psz[32];
            format_size(info.partitions[i].size, psz, sizeof(psz));
            printf("#%d %s %s", info.partitions[i].number,
                   fs_type_name(info.partitions[i].fs_type), psz);
        }
        if (info.num_partitions == 0) printf("(none)");
        printf("\n");
    }
    closedir(d);
    return 0;
}

int disk_list_one(const char *dev_path)
{
    disk_info_t info;
    int fd = disk_open(dev_path, &info);
    if (fd < 0) return -1;

    partition_scan(fd, &info);
    disk_close(fd);

    char sz[32];
    format_size(info.size, sz, sizeof(sz));
    const char *pt = "unknown";
    if (info.pt_type == PT_MBR) pt = "MBR";
    else if (info.pt_type == PT_GPT) pt = "GPT";

    printf("\nDisk: %s\n", info.dev_path);
    printf("  Model:       %s\n", info.model[0] ? info.model : "-");
    printf("  Size:        %s (%llu bytes)\n", sz, (unsigned long long)info.size);
    printf("  Table:       %s\n", pt);
    printf("  Partitions:  %d\n\n", info.num_partitions);

    if (info.num_partitions == 0) {
        printf("  (no partitions found)\n");
        return 0;
    }

    printf("  %-4s %-20s %-8s %12s  %-16s %s\n",
           "#", "Device", "Type", "Size", "Label", "UUID");
    printf("  %-4s %-20s %-8s %12s  %-16s %s\n",
           "---", "--------------------", "--------", "------------", "----------------", "----");

    for (int i = 0; i < info.num_partitions; i++) {
        partition_info_t *p = &info.partitions[i];
        format_size(p->size, sz, sizeof(sz));
        printf("  %-4d %-20s %-8s %12s  %-16s %s\n",
               p->number, p->dev_path, fs_type_name(p->fs_type),
               sz, p->fs_label, p->fs_uuid);
    }
    printf("\n");
    return 0;
}
