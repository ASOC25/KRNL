#!/usr/bin/env bash
set -euo pipefail

# Configurable parameters via environment variables
: "${IMG:=build/disk.img}"
: "${IMG_SIZE_MB:=64}"
: "${MTOOLS_RC:=.mtoolsrc}"

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
LIMINE_DIR="$ROOT_DIR/limine"
OVMF_DIR="$ROOT_DIR/OVMFbin"

KERNEL_ELF="$BUILD_DIR/kernel.elf"
STARTUP_NSH="$ROOT_DIR/startup.nsh"
BOOTX64_EFI="$LIMINE_DIR/BOOTX64.EFI"
LIMINE_CFG="$ROOT_DIR/limine.conf"

usage() {
  cat <<EOF
Usage: $0 [--img path] [--size MB] [--force]

Creates a GPT disk image with a single FAT EFI System Partition
containing:
  - EFI/BOOT/BOOTX64.EFI (copied from limine/BOOTX64.EFI)
  - startup.nsh
  - kernel.elf
  - limine.conf

Environment overrides:
  IMG (default build/disk.img)
  IMG_SIZE_MB (default 64)

Options:
  --img PATH     Output image path (default: $IMG)
  --size MB      Image size in MB (default: $IMG_SIZE_MB)
  --force        Overwrite existing image
  -h, --help     Show this help
EOF
}

FORCE=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --img)
      IMG="$2"; shift 2;;
    --size)
      IMG_SIZE_MB="$2"; shift 2;;
    --force)
      FORCE=1; shift;;
    -h|--help)
      usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1;;
  esac
done

if [[ ! -f "$KERNEL_ELF" ]]; then
  echo "Kernel not found at $KERNEL_ELF. Build it first." >&2
  exit 1
fi
if [[ ! -f "$STARTUP_NSH" ]]; then
  echo "startup.nsh not found at $STARTUP_NSH" >&2
  exit 1
fi
if [[ ! -f "$BOOTX64_EFI" ]]; then
  echo "BOOTX64.EFI not found at $BOOTX64_EFI" >&2
  exit 1
fi
if [[ ! -f "$LIMINE_CFG" ]]; then
  echo "limine.conf not found at $LIMINE_CFG" >&2
  exit 1
fi


mkdir -p "$(dirname "$IMG")"

if [[ -e "$IMG" && $FORCE -ne 1 ]]; then
  echo "Image $IMG already exists. Use --force to overwrite." >&2
  exit 1
fi

# Remove existing
rm -f "$IMG"

# Create empty image
truncate -s "${IMG_SIZE_MB}M" "$IMG"

# Create GPT with a single EFI System Partition (type EF00)
# We'll align at 1MB, size rest of disk
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart EFI FAT32 1MiB 100%
parted -s "$IMG" set 1 esp on

# Map the partition using loop device
LOOPDEV=$(losetup --find --partscan --show "$IMG")
PARTITION="${LOOPDEV}p1"

cleanup() {
  set +e
  sync
  if mountpoint -q mnt-esp; then sudo umount mnt-esp; fi
  if [[ -n "${LOOPDEV:-}" ]]; then sudo losetup -d "$LOOPDEV"; fi
}
trap cleanup EXIT

# Wait for partition node
for _ in {1..10}; do
  [[ -b "$PARTITION" ]] && break
  sleep 0.2
done

if [[ ! -b "$PARTITION" ]]; then
  echo "Partition device $PARTITION not found" >&2
  exit 1
fi

# Format as FAT32
sudo mkfs.vfat -F32 -n EFI "$PARTITION" > /dev/null

mkdir -p mnt-esp
sudo mount "$PARTITION" mnt-esp

sudo mkdir -p mnt-esp/EFI/BOOT
sudo cp "$BOOTX64_EFI" mnt-esp/EFI/BOOT/BOOTX64.EFI
sudo cp "$STARTUP_NSH" mnt-esp/startup.nsh
sudo cp "$KERNEL_ELF" mnt-esp/kernel.elf
sudo cp "$LIMINE_CFG" mnt-esp/limine.conf

sync
sudo umount mnt-esp
rmdir mnt-esp
sudo losetup -d "$LOOPDEV"
trap - EXIT

echo "Created EFI disk image: $IMG"

echo "Run with (example):"
echo "  qemu-system-x86_64 -machine q35 -m 512 \\
     -drive if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_DIR/OVMF_CODE-pure-efi.fd \\
     -drive if=pflash,format=raw,unit=1,file=$OVMF_DIR/OVMF_VARS-pure-efi.fd \\
     -drive format=raw,file=$IMG"