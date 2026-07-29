#!/usr/bin/env bash
set -euo pipefail

# Configurable parameters via environment variables
: "${IMG:=build/disk.img}"
: "${IMG_SIZE_MB:=768}"

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
LIMINE_DIR="$ROOT_DIR/limine"
ENV_DIR="$ROOT_DIR/env"

KERNEL_ELF="$BUILD_DIR/kernel.elf"
STARTUP_NSH="$ENV_DIR/startup.nsh"
BOOTX64_EFI="$ENV_DIR/BOOTX64.EFI"
LIMINE_CFG="$ENV_DIR/limine.conf"

usage() {
  cat <<EOF
Usage: $0 [--img path] [--size MB] [--force]

Creates a GPT disk image with a single FAT EFI System Partition
containing:
  - EFI/BOOT/BOOTX64.EFI (copied from env/BOOTX64.EFI)
  - startup.nsh
  - kernel.elf
  - limine.conf

Environment overrides:
  IMG (default build/disk.img)
  IMG_SIZE_MB (default 768)

Options:
  --img PATH     Output image path (default: \$IMG)
  --size MB      Image size in MB (default: \$IMG_SIZE_MB)
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

for f in "$KERNEL_ELF" "$STARTUP_NSH" "$BOOTX64_EFI" "$LIMINE_CFG"; do
  if [[ ! -f "$f" ]]; then
    echo "Required file not found: $f" >&2
    exit 1
  fi
done

mkdir -p "$(dirname "$IMG")"

if [[ -e "$IMG" && $FORCE -ne 1 ]]; then
  echo "Image $IMG already exists. Use --force to overwrite." >&2
  exit 1
fi

rm -f "$IMG"

# Create raw image
truncate -s "${IMG_SIZE_MB}M" "$IMG"

# Partition: GPT, single ESP starting at 1MiB
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart EFI FAT32 1MiB 100%
parted -s "$IMG" set 1 esp on

# Compute partition byte offset (parted reports start in sectors; 1 sector = 512 bytes)
SECTOR=$(parted -s "$IMG" unit s print | awk '/^ 1/{print $2}' | tr -d 's')
OFFSET=$(( SECTOR * 512 ))

# Format FAT32 and populate using mtools (no loop device / no root needed)
MIMG="${IMG}@@${OFFSET}"
mformat -i "$MIMG" -F -v EFI -c 1 ::
mmd    -i "$MIMG" ::/EFI ::/EFI/BOOT
mcopy  -i "$MIMG" "$BOOTX64_EFI"  ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$MIMG" "$STARTUP_NSH"  ::/startup.nsh
mcopy  -i "$MIMG" "$KERNEL_ELF"   ::/kernel.elf
mcopy  -i "$MIMG" "$LIMINE_CFG"   ::/limine.conf

echo "Created EFI disk image: $IMG"
