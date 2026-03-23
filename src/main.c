/*
 * disk2vmdk — Linux disk imaging tool
 *
 * Usage:
 *   disk2vmdk list [<disk>]                           — list disks or one disk's partitions
 *   disk2vmdk make <disk> -o <file> [options]         — create disk image
 *
 * Partition specifiers: device paths (nvme0n1p2, /dev/sda3) or numbers (2, 3).
 *
 * Examples:
 *   disk2vmdk list
 *   disk2vmdk list /dev/nvme0n1
 *   disk2vmdk make /dev/sda -o server.vmdk --exclude /dev/sda3 --used-only /dev/sda1
 *   disk2vmdk make /dev/nvme0n1 -o backup.vhd --exclude nvme0n1p2 --used-only-all
 *   disk2vmdk make /dev/sda -o backup.vmdk --exclude 3 --used-only 1,2
 */
#include "common.h"
#include <getopt.h>

static void print_usage(void)
{
    fprintf(stderr,
        "disk2vmdk v" D2V_VERSION " — Linux disk imaging tool\n"
        "\n"
        "Usage:\n"
        "  disk2vmdk -i <disk>                              Interactive mode\n"
        "  disk2vmdk list [<disk>]                          List all disks\n"
        "  disk2vmdk make <disk> -o <file> [options]        Create disk image\n"
        "\n"
        "Options:\n"
        "  -o <file>              Output file (.vmdk .vhd .vdi .dd .raw .img)\n"
        "  -f <format>            Force format: vmdk, vhd, vdi, dd\n"
        "  -i                     Interactive mode (TUI for partition selection)\n"
        "  --exclude <N,...>      Exclude partition numbers\n"
        "  --used-only <P,...>    Used-only mode for partitions (ext4/xfs)\n"
        "  --used-only-all        Used-only mode for all supported partitions\n"
        "  --buf-size <MB>        IO buffer size (default: 8)\n"
        "\n"
        "Examples:\n"
        "  disk2vmdk -i /dev/sda\n"
        "  disk2vmdk list\n"
        "  disk2vmdk make /dev/sda -o server.vmdk --exclude 3 --used-only 1,2\n"
    );
}

/* ========================================================================= */
/*  Partition matching: support device paths, short names, and numbers        */
/* ========================================================================= */

static bool partition_matches(const partition_info_t *p, const char *spec)
{
    /* Try as number first */
    if (spec[0] >= '0' && spec[0] <= '9') {
        int num = atoi(spec);
        return p->number == num;
    }

    /* Full device path match */
    if (strcmp(p->dev_path, spec) == 0)
        return true;

    /* Match without /dev/ prefix */
    if (spec[0] != '/') {
        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "/dev/%s", spec);
        if (strcmp(p->dev_path, fullpath) == 0)
            return true;
    }

    /* Match just the basename */
    const char *p_base = strrchr(p->dev_path, '/');
    if (p_base) p_base++; else p_base = p->dev_path;
    const char *s_base = strrchr(spec, '/');
    if (s_base) s_base++; else s_base = spec;
    return strcmp(p_base, s_base) == 0;
}

static bool partition_in_list(const partition_info_t *p, const char *list_str)
{
    if (!list_str || !list_str[0]) return false;

    char buf[4096];
    strncpy(buf, list_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        if (partition_matches(p, tok))
            return true;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return false;
}

/* Check if a string looks like a device name/path */
static bool looks_like_device(const char *s)
{
    if (s[0] == '/') return true;
    if (strncmp(s, "sd", 2) == 0) return true;
    if (strncmp(s, "nvme", 4) == 0) return true;
    if (strncmp(s, "vd", 2) == 0) return true;
    if (strncmp(s, "hd", 2) == 0) return true;
    if (strncmp(s, "mmcblk", 6) == 0) return true;
    return false;
}

/* Prepend /dev/ if needed */
static const char *ensure_dev_path(const char *s, char *buf, size_t buflen)
{
    if (s[0] == '/') return s;
    snprintf(buf, buflen, "/dev/%s", s);
    return buf;
}

/* ========================================================================= */
/*  Command: list                                                             */
/* ========================================================================= */

static int cmd_list(int argc, char **argv)
{
    const char *dev = NULL;
    for (int i = 0; i < argc; i++) {
        if (looks_like_device(argv[i])) { dev = argv[i]; break; }
    }

    if (dev) {
        char fullpath[MAX_PATH_LEN];
        return disk_list_one(ensure_dev_path(dev, fullpath, sizeof(fullpath)));
    }

    return disk_list_all();
}

/* ========================================================================= */
/*  Command: make                                                             */
/* ========================================================================= */

static int cmd_make(int argc, char **argv)
{
    const char *disk_path = NULL;
    const char *output = NULL;
    const char *format_str = NULL;
    const char *exclude_str = NULL;
    const char *used_only_str = NULL;
    bool used_only_all = false;
    int buf_mb = 8;

    static struct option long_opts[] = {
        { "exclude",        required_argument, 0, 'e' },
        { "used-only",      required_argument, 0, 'u' },
        { "used-only-all",  no_argument,       0, 'U' },
        { "buf-size",       required_argument, 0, 'B' },
        { "full",           no_argument,       0, 'F' },
        { "help",           no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    optind = 0;
    int c;
    while ((c = getopt_long(argc, argv, "o:f:e:u:B:h", long_opts, NULL)) != -1) {
        switch (c) {
        case 'o': output = optarg; break;
        case 'f': format_str = optarg; break;
        case 'e': exclude_str = optarg; break;
        case 'u': used_only_str = optarg; break;
        case 'U': used_only_all = true; break;
        case 'B': buf_mb = atoi(optarg); break;
        case 'F': break;
        case 'h': print_usage(); return 0;
        default:  print_usage(); return 1;
        }
    }

    /* Remaining args: look for disk device */
    {
        int i;
        for (i = optind; i < argc; i++) {
            if (!disk_path && looks_like_device(argv[i]))
                disk_path = argv[i];
        }
    }

    if (!disk_path) {
        fprintf(stderr, "Error: no disk device specified\n\n");
        print_usage();
        return 1;
    }
    if (!output) {
        fprintf(stderr, "Error: no output file specified (-o)\n\n");
        print_usage();
        return 1;
    }

    char fullpath[MAX_PATH_LEN];
    disk_path = ensure_dev_path(disk_path, fullpath, sizeof(fullpath));

    /* Detect format */
    vdisk_format_t fmt;
    if (format_str)
        fmt = vdisk_format_from_ext(format_str);
    else {
        const char *ext = strrchr(output, '.');
        fmt = vdisk_format_from_ext(ext);
    }

    /* Open disk */
    disk_info_t disk;
    int fd = disk_open(disk_path, &disk);
    if (fd < 0) return 1;

    if (partition_scan(fd, &disk) < 0) {
        fprintf(stderr, "Error: failed to scan partitions\n");
        disk_close(fd);
        return 1;
    }

    char sz[32];
    format_size(disk.size, sz, sizeof(sz));
    fprintf(stderr, "Disk: %s  %s  %s  %d partitions\n\n",
            disk.dev_path, disk.model[0] ? disk.model : "", sz, disk.num_partitions);

    /* Apply exclude and used-only settings */
    for (int i = 0; i < disk.num_partitions; i++) {
        partition_info_t *p = &disk.partitions[i];
        format_size(p->size, sz, sizeof(sz));

        if (partition_in_list(p, exclude_str)) {
            p->selected = false;
            fprintf(stderr, "  %-20s  %-8s  %10s  %-16s  [EXCLUDED]\n",
                    p->dev_path, fs_type_name(p->fs_type), sz, p->fs_label);
        }
        else if (used_only_all || partition_in_list(p, used_only_str)) {
            if (p->fs_type == FS_EXT4 || p->fs_type == FS_EXT3 ||
                p->fs_type == FS_EXT2 || p->fs_type == FS_XFS ||
                p->fs_type == FS_LVM) {
                p->copy_mode = 1;
                fprintf(stderr, "  %-20s  %-8s  %10s  %-16s  [USED-ONLY]\n",
                        p->dev_path, fs_type_name(p->fs_type), sz, p->fs_label);
            } else {
                fprintf(stderr, "  %-20s  %-8s  %10s  %-16s  [FULL — %s not supported for used-only]\n",
                        p->dev_path, fs_type_name(p->fs_type), sz, p->fs_label,
                        fs_type_name(p->fs_type));
            }
        }
        else {
            fprintf(stderr, "  %-20s  %-8s  %10s  %-16s  [FULL]\n",
                    p->dev_path, fs_type_name(p->fs_type), sz, p->fs_label);
        }
    }
    fprintf(stderr, "\n");

    /* Run imaging */
    imaging_config_t cfg = {
        .disk = &disk,
        .disk_fd = fd,
        .output_path = output,
        .format = fmt,
        .progress = progress_print,
        .buf_size = (uint32_t)buf_mb * 1024 * 1024
    };

    int rc = imaging_run(&cfg);
    disk_close(fd);
    return rc;
}

/* ========================================================================= */
/*  Command: interactive (-i)                                                 */
/* ========================================================================= */

static int cmd_interactive(int argc, char **argv)
{
    const char *disk_path = NULL;
    const char *output_preset = NULL;
    int i;

    /* Parse arguments: look for device and -o */
    for (i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
            output_preset = argv[++i];
        } else if (looks_like_device(argv[i])) {
            if (!disk_path) disk_path = argv[i];
        }
    }
    if (!disk_path) {
        fprintf(stderr, "Error: no disk device specified\n");
        fprintf(stderr, "Usage: disk2vmdk -i <disk> [-o output.vmdk]\n");
        return 1;
    }

    char fullpath[MAX_PATH_LEN];
    disk_path = ensure_dev_path(disk_path, fullpath, sizeof(fullpath));

    disk_info_t disk;
    int fd = disk_open(disk_path, &disk);
    if (fd < 0) return 1;

    if (partition_scan(fd, &disk) < 0) {
        fprintf(stderr, "Error: failed to scan partitions\n");
        disk_close(fd);
        return 1;
    }

    imaging_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int tui_rc = tui_run(fd, &disk, output_preset, &cfg);
    if (tui_rc != 0) {
        disk_close(fd);
        return tui_rc < 0 ? 1 : 0;  /* -1=error, 1=cancelled (success exit) */
    }

    int rc = imaging_run(&cfg);

    /* Free strdup'd output_path from tui */
    free((void *)cfg.output_path);
    disk_close(fd);
    return rc;
}

/* ========================================================================= */
/*  Main                                                                      */
/* ========================================================================= */

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (geteuid() != 0)
        fprintf(stderr, "Warning: not running as root — disk access may fail.\n\n");

    if (strcmp(cmd, "-i") == 0 || strcmp(cmd, "--interactive") == 0)
        return cmd_interactive(argc - 2, argv + 2);
    else if (strcmp(cmd, "list") == 0)
        return cmd_list(argc - 2, argv + 2);
    else if (strcmp(cmd, "make") == 0)
        return cmd_make(argc - 1, argv + 1);
    else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        print_usage();
        return 0;
    }
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        print_usage();
        return 1;
    }
}
