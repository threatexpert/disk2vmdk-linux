# disk2vmdk

A fast Linux disk imaging tool for incident response. Creates VMDK/VHD/VDI virtual disk images directly from physical disks, with filesystem-aware sparse copying to minimize image size and creation time.

## Features

- **Multiple output formats**: VMDK (VMware), VHD (Hyper-V), VDI (VirtualBox), DD/RAW
- **Filesystem-aware sparse copy**: Only copies used blocks by reading the filesystem's block allocation bitmap, dramatically reducing image size and time
- **LVM support**: Automatically detects LVM physical volumes, scans logical volumes via device-mapper, and reads bitmaps from each LV's filesystem
- **Partition-level control**: Exclude partitions (e.g., large data volumes) or selectively enable used-only mode per partition
- **Supported filesystems**: ext4/ext3/ext2 (block bitmap), XFS (free space B+tree), LVM (with ext4/XFS inside LVs)
- **Zero dependencies**: Single static binary (~1MB), compiles with just `gcc`. No libraries, no runtime dependencies. Copy to any Linux machine and run
- **Partition table support**: GPT and MBR (including logical partitions in extended partitions)

## Quick Start

```bash
# Build
./build.sh

# Interactive mode (recommended)
sudo ./disk2vmdk -i /dev/sda

# List disks
sudo ./disk2vmdk list

# List partitions on a specific disk
sudo ./disk2vmdk list /dev/sda

# Create VMDK image (full copy)
sudo ./disk2vmdk make /dev/sda -o server.vmdk

# Create VMDK, only copy used blocks on all supported partitions
sudo ./disk2vmdk make /dev/sda -o server.vmdk --used-only-all

# Exclude a large data partition, used-only for the rest
sudo ./disk2vmdk make /dev/sda -o server.vmdk --exclude /dev/sda3 --used-only-all

# Output VHD format
sudo ./disk2vmdk make /dev/nvme0n1 -o backup.vhd --used-only-all
```

## Usage

```
disk2vmdk — Linux disk imaging tool

Usage:
  disk2vmdk list [<disk>]                          List all disks, or one disk in detail
  disk2vmdk make <disk> -o <file> [options]        Create disk image

Options:
  -o <file>              Output file (.vmdk .vhd .vdi .dd .raw .img)
  -f <format>            Force format: vmdk, vhd, vdi, dd
  --exclude <P,...>      Exclude partitions (device path or number)
  --used-only <P,...>    Used-only mode for partitions (ext4/xfs/lvm)
  --used-only-all        Used-only mode for all supported partitions
  --buf-size <MB>        IO buffer size (default: 8)
```

Partition specifiers accept device paths (`/dev/sda3`, `sda3`, `nvme0n1p2`) or partition numbers (`3`).

## Example Output

```
$ sudo ./disk2vmdk make /dev/nvme0n1 -o server.vmdk --used-only-all

Disk: /dev/nvme0n1  VMware Virtual NVMe Disk  21.47 GB  2 partitions

  /dev/nvme0n1p1        xfs          1.07 GB                    [USED-ONLY]
  /dev/nvme0n1p2        lvm         20.40 GB                    [USED-ONLY]

Creating VMDK image: server.vmdk
Source: /dev/nvme0n1 (21.47 GB)
  Copying gap [0 - 1048576] (1048576 bytes)
  Partition #1 (xfs): used-only mode
  xfs bitmap: 83937/262144 blocks used (343.81 MB / 1.07 GB, block_size=4096)
  Partition #2 (lvm): used-only mode
    LVM: 2 LVs on /dev/nvme0n1p2 (metadata: 2048 sectors)
    LV cs-root               xfs
  xfs bitmap: 1560564/4455424 blocks used (6.39 GB / 18.25 GB, block_size=4096)
    LV cs-swap               swap
      swap: skipping (only header marked)
    LVM combined: 12486568/39843840 sectors used (6.39 GB / 20.40 GB)
  [==============================] 100%  21.47 GB/21.47 GB  data:6.74 GB  240 MB/s
Done. 6.74 GB data written in 26.8 seconds (240 MB/s)
```

A 21.5 GB disk with 6.1 GB of actual data produces a ~6.7 GB VMDK in 27 seconds.

## How It Works

### Disk Reading
The tool reads the entire disk (`/dev/sda`, `/dev/nvme0n1`, etc.) sequentially. For each partition:
- **Excluded partitions**: Written as zeros (sparse — skipped in the output format)
- **Full copy**: All sectors are read and written
- **Used-only**: The filesystem's allocation bitmap is read to determine which blocks contain data. Only used blocks are copied; free blocks are written as zeros (sparse skip)

### Filesystem Bitmap Reading
All bitmap reading is done by **directly parsing on-disk structures** — no mount required, no library dependencies.

- **ext4/ext3/ext2**: Reads the superblock → group descriptor table → block bitmap for each block group
- **XFS**: Reads the superblock → per-AG AGF header → walks the by-block free space B+tree (bnobt) to collect free extents
- **LVM**: Runs `dmsetup table` to discover logical volumes and their physical extent mapping, then reads the bitmap from each LV's filesystem and maps it back to the partition's physical sector offsets

### Output Formats
- **VMDK** (monolithicSparse): Grain-based (64KB), with redundant grain directory for VMware compatibility. Sequential write with 2MB batched IO
- **VHD** (Dynamic): 2MB block-based with BAT (Block Allocation Table)
- **VDI** (Dynamic): 1MB block-based with block map

All formats support sparse/dynamic allocation — zero regions are not stored in the output file.

## Building

Requirements: `gcc` (any version supporting C99)

```bash
# Standard build (tries static first, falls back to dynamic)
./build.sh

# For static build on CentOS/RHEL, install glibc-static:
sudo yum install glibc-static
./build.sh
```

The static binary has zero runtime dependencies and can be copied to any Linux x86_64 machine.

### Cross-compilation
```bash
# Build on Ubuntu for deployment on CentOS 7+
gcc -std=gnu99 -O2 -static -o disk2vmdk src/*.c -lpthread
```

## Typical Use Cases

### Incident Response — Image a Server's System Disk
```bash
# Skip the 2TB data partition, only copy used blocks from system partitions
sudo ./disk2vmdk make /dev/sda -o evidence.vmdk \
    --exclude /dev/sda4 \
    --used-only-all
```

### Forensic Copy — Full Disk Image
```bash
# Full bit-for-bit copy in raw format
sudo ./disk2vmdk make /dev/sda -o evidence.dd -f dd
```

### CentOS/RHEL with LVM
```bash
# LVM is automatically detected and handled
# If running from Live USB, activate LVM first:
sudo vgchange -ay

sudo ./disk2vmdk make /dev/sda -o server.vmdk --used-only-all
```

## LVM Notes

For `--used-only` to work on LVM partitions, the volume group must be activated (LVs visible under `/dev/mapper/`). On a running system this is already the case. From a Live USB:

```bash
sudo vgscan
sudo vgchange -ay
sudo ./disk2vmdk make /dev/sda -o server.vmdk --used-only-all
```

If `dmsetup` is not available or LVM is not activated, the tool will fall back and report an error — it will **not** silently produce an incomplete image.

## Error Handling

The tool follows a **fail-fast** philosophy for data integrity:
- If bitmap reading fails for any partition in used-only mode, the tool **aborts immediately** rather than silently producing an incomplete image
- The error message tells you exactly which partition failed and suggests removing `--used-only` for that partition
- You can always fall back to full copy mode for specific partitions while keeping used-only for others

## Interactive Mode

Run with `-i` for a terminal UI where you can visually select partitions, toggle copy modes, and set the output file — no need to remember command-line flags:

```bash
sudo ./disk2vmdk -i /dev/sda
```

```
disk2vmdk v1.2.0 — Interactive Mode

Disk: /dev/nvme0n1  VMware Virtual NVMe Disk  21.47 GB  MBR

  Sel Device               Type         Size  Label            Copy Mode
  ──────────────────────────────────────────────────────────────────────
  ✓  /dev/nvme0n1p1       xfs        1.07 GB                  Used-only
  ✓  /dev/nvme0n1p2       lvm       20.40 GB                  Used-only

  Output file:  server.vmdk
  Format:       [VMDK]   VHD    VDI    DD

  ▶ START      QUIT

  ↑↓ Navigate   Space: toggle select/mode   Tab: next field   Enter: confirm   q: quit
```

Use arrow keys and Tab to navigate between fields, Space to toggle selections, Enter to start.

## Project Structure

```
disk2vmdk-linux/
├── build.sh              Build script
├── src/
│   ├── common.h          Types, constants, module interfaces
│   ├── main.c            CLI: list / make commands
│   ├── disk.c            Disk enumeration, open, sysfs info
│   ├── partition.c       GPT + MBR (with EBR chain) parsing, filesystem detection
│   ├── bitmap_ext4.c     ext4 block bitmap reader (raw on-disk parsing)
│   ├── bitmap_xfs.c      XFS free space B+tree reader (raw on-disk parsing)
│   ├── lvm.c             LVM PV/LV awareness via dmsetup
│   ├── vdisk_writer.c    VMDK / VHD / VDI / DD output writer
│   ├── imaging.c         Orchestrator: reader thread → buffer queue → writer thread
│   ├── progress.c        Terminal progress bar
│   └── tui.c             Interactive terminal UI (pure ANSI, no ncurses)
└── README.md
```

~3,500 lines of C. No third-party dependencies.

## License

MIT

## Changelog

### v1.2.0
- **Interactive terminal UI**: `disk2vmdk -i <disk>` opens a visual interface for selecting partitions, copy modes, output file, and format — no flags to remember
- Pure ANSI escape codes, no ncurses dependency

### v1.1.0
- **Parallel read/write pipeline**: Reader thread reads disk and writer thread (main) writes to vdisk concurrently via buffer queue, improving throughput on fast storage
- Version info displayed in `--help` output

### v1.0.0
- Initial release
- VMDK / VHD / VDI / DD output formats
- ext4 and XFS bitmap-aware sparse copy
- LVM PV/LV support via dmsetup
- GPT + MBR (with EBR logical partitions) support
- Partition exclude and used-only modes
