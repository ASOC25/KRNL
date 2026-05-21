#!/bin/sh
# create-ext2.sh <sysroot> <output-image> [size-mb]
# Creates an ext2 filesystem image populated with a standard Unix directory
# structure from <sysroot>.  Tries genext2fs (no root needed) first;
# falls back to mke2fs + loop mount (requires sudo).

set -e

SYSROOT="${1:?Usage: create-ext2.sh <sysroot> <output> [size-mb]}"
OUTPUT="${2:?Usage: create-ext2.sh <sysroot> <output> [size-mb]}"
SIZE_MB="${3:-128}"

# Ensure the standard Unix directory skeleton exists inside sysroot
for d in bin sbin etc dev proc sys tmp home root lib usr usr/bin usr/sbin usr/lib usr/share var var/log var/tmp run; do
    mkdir -p "$SYSROOT/$d"
done

if command -v genext2fs >/dev/null 2>&1; then
    echo "Using genext2fs (no root required)..."
    BLOCKS=$((SIZE_MB * 1024))
    # Generous inode count: 1 inode per 2 KB of image
    INODES=$((BLOCKS / 2))
    genext2fs \
        -b "$BLOCKS" \
        -N "$INODES" \
        -d "$SYSROOT" \
        -L "KRNL_ROOT" \
        "$OUTPUT"
    echo "Ext2 image (genext2fs): $OUTPUT  (${SIZE_MB} MB)"
else
    echo "genext2fs not found, using mke2fs + loop mount (requires sudo)..."
    dd if=/dev/zero of="$OUTPUT" bs=1M count="$SIZE_MB" status=none
    mke2fs -t ext2 -b 4096 -L "KRNL_ROOT" -m 0 -q "$OUTPUT"
    MNT=$(mktemp -d)
    sudo mount -o loop "$OUTPUT" "$MNT"
    # Re-create skeleton on the mounted image
    for d in bin sbin etc dev proc sys tmp home root lib usr usr/bin usr/sbin usr/lib usr/share var var/log var/tmp run; do
        sudo mkdir -p "$MNT/$d"
    done
    sudo cp -rp "$SYSROOT/." "$MNT/"
    sudo umount "$MNT"
    rmdir "$MNT"
    echo "Ext2 image (mke2fs):    $OUTPUT  (${SIZE_MB} MB)"
fi
