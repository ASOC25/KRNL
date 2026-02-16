#!/usr/bin/env bash
set -euo pipefail

# cleanup-loops.sh
# Safely clean up stale loop devices & mounts that may remain if image
# creation (make-efi-img.sh) was interrupted before losetup -d.
# Requires sudo for unmounting and detaching loop devices.

# Strategy:
# 1. Detect mount points matching pattern mnt-esp under repo root (default) and unmount.
# 2. Optionally force detach loop devices whose backing file is an image in build/ (disk.img by default) if not in use.
# 3. Provide dry-run mode.

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
DEFAULT_IMG="$BUILD_DIR/disk.img"

: "${IMG_PATTERN:=$BUILD_DIR/*.img}"
: "${DRY_RUN:=no}"   # yes -> show actions only
: "${FORCE:=no}"     # yes -> detach even if not sure (careful)

usage() {
  cat <<EOF
Usage: $0 [--dry-run] [--force] [--pattern 'glob']

Environment overrides:
  IMG_PATTERN   Glob of image files to inspect (default: $BUILD_DIR/*.img)
  DRY_RUN       yes to only print actions
  FORCE         yes to attempt forced loop detach (use cautiously)

Actions performed:
  - Unmount stale mnt-esp mounts inside repo root if present
  - List loop devices referencing matching image files
  - Detach them if safe (or if --force)

Examples:
  $0                 # Normal cleanup
  DRY_RUN=yes $0     # Preview actions
  IMG_PATTERN='build/disk*.img' $0
  FORCE=yes $0       # Aggressive (only if you are sure)
EOF
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --dry-run) DRY_RUN=yes; shift;;
    --force) FORCE=yes; shift;;
    --pattern) IMG_PATTERN="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1;;
  esac
done

run() {
  if [[ $DRY_RUN == yes ]]; then
    echo "DRY-RUN: $*"
  else
    echo "+ $*"
    eval "$@"
  fi
}

# 1. Unmount stale mnt-esp
if mountpoint -q "$ROOT_DIR/mnt-esp"; then
  run "sudo umount '$ROOT_DIR/mnt-esp'"
fi
if [[ -d "$ROOT_DIR/mnt-esp" && ! $(mount | grep -F "$ROOT_DIR/mnt-esp") ]]; then
  run "rmdir '$ROOT_DIR/mnt-esp' || true"
fi

# 2. Loop devices referencing image(s)
shopt -s nullglob
IMGS=( $IMG_PATTERN )
shopt -u nullglob

if [[ ${#IMGS[@]} -eq 0 ]]; then
  echo "No images matching pattern $IMG_PATTERN"
else
  echo "Scanning loop devices for: ${IMGS[*]}"
  while read -r line; do
    # Format: /dev/loopX: [ref] (/path)
    dev=$(echo "$line" | awk -F: '{print $1}')
    file=$(echo "$line" | sed -E 's|.*\((/.*)\).*|\1|')
    for img in "${IMGS[@]}"; do
      if [[ "$file" == "$img" ]]; then
        echo "Found loop device $dev -> $file"
        # Check if mounted
        if mount | grep -q "^$dev"; then
          echo "  Still mounted; attempting lazy unmounts referencing $dev"
          mps=$(mount | awk -v d="$dev" '$1==d {print $3}')
          for mp in $mps; do
            run "sudo umount '$mp'" || true
          done
        fi
        if [[ $FORCE == yes ]]; then
          run "sudo losetup -d '$dev'" || true
        else
          # Only detach if not listed in mount anymore
            if ! mount | grep -q "^$dev"; then
              run "sudo losetup -d '$dev'" || echo "  (Could not detach $dev)"
            else
              echo "  Skipping detach of $dev (still appears mounted)"
            fi
        fi
      fi
    done
  done < <(losetup -a || true)
fi

echo "Cleanup complete."