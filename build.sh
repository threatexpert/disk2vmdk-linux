#!/bin/bash
# Build script for disk2vmdk-linux
# Usage: ./build.sh
# Output: disk2vmdk (single static binary)

set -e

SRC="src"
OUT="disk2vmdk"

SOURCES="
    $SRC/main.c
    $SRC/disk.c
    $SRC/partition.c
    $SRC/bitmap_ext4.c
    $SRC/bitmap_xfs.c
    $SRC/lvm.c
    $SRC/vdisk_writer.c
    $SRC/imaging.c
    $SRC/progress.c
"

CFLAGS="-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64"
LDFLAGS="-lpthread"

# Try static linking first (for portability)
echo "Building disk2vmdk..."
if gcc $CFLAGS -static -o $OUT $SOURCES $LDFLAGS 2>/dev/null; then
    echo "Built static binary: $OUT"
else
    echo "Static build failed, trying dynamic..."
    gcc $CFLAGS -o $OUT $SOURCES $LDFLAGS
    echo "Built dynamic binary: $OUT"
fi

ls -lh $OUT
echo "Done."
