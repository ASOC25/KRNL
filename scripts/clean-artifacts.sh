#!/usr/bin/env bash
set -euo pipefail

# clean-artifacts.sh
# Cleans build artifacts. Two levels:
#  - normal (default): remove build outputs (build/, *.img, *.iso) but keep tool/firmware assets
#  - dist (distribution clean): also remove downloaded/embedded binary blobs (OPTIONAL)
# Currently OVMFbin/ and limine/ are kept even on dist unless DIST_EXTRAS=yes.

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
RAMDISK_DIR="$ROOT_DIR/ramdisk"

: "${MODE:=normal}"          # normal|dist
: "${DIST_EXTRAS:=no}"       # yes -> also remove OVMFbin & limine directories
: "${DRY_RUN:=no}"

usage() {
  cat <<EOF
Usage: $0 [--mode normal|dist] [--dist-extras] [--dry-run]

Environment variables:
  MODE=normal|dist      Cleaning mode (default: normal)
  DIST_EXTRAS=yes       In dist mode also remove OVMFbin/ and limine/
  DRY_RUN=yes           Show what would be removed

Examples:
  $0                    # Normal clean
  MODE=dist $0          # Dist clean (keeps OVMFbin limine)
  MODE=dist DIST_EXTRAS=yes $0  # Aggressive dist clean
  DRY_RUN=yes $0        # Preview
EOF
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --mode) MODE="$2"; shift 2;;
    --dist-extras) DIST_EXTRAS=yes; shift;;
    --dry-run) DRY_RUN=yes; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1;;
  esac
done

if [[ "$MODE" != normal && "$MODE" != dist ]]; then
  echo "Invalid MODE: $MODE" >&2; exit 1
fi

# Collect removal targets
REMOVE=( )

# Build artifacts
[[ -d "$BUILD_DIR" ]] && REMOVE+=( "$BUILD_DIR" )
# Ramdisk contents (but keep .gitkeep files if present)
if [[ -d "$RAMDISK_DIR" ]]; then
  shopt -s nullglob
  for entry in "$RAMDISK_DIR"/* "$RAMDISK_DIR"/.*; do
    name=$(basename -- "$entry")
    # Skip current/parent dirs and any .gitkeep sentinel
    if [[ "$name" == "." || "$name" == ".." || "$name" == ".gitkeep" ]]; then
      continue
    fi
    REMOVE+=( "$entry" )
  done
  shopt -u nullglob
fi
# Common generated files at root
shopt -s nullglob
for f in *.img *.iso *.log core core.*; do
  [[ -e $f ]] && REMOVE+=( "$ROOT_DIR/$f" )
done
shopt -u nullglob

if [[ $MODE == dist ]]; then
  # Optionally also remove firmware & limine directories (user's choice)
  if [[ $DIST_EXTRAS == yes ]]; then
    [[ -d "$ROOT_DIR/OVMFbin" ]] && REMOVE+=( "$ROOT_DIR/OVMFbin" )
    [[ -d "$ROOT_DIR/limine" ]] && REMOVE+=( "$ROOT_DIR/limine" )
  fi
fi

if [[ ${#REMOVE[@]} -eq 0 ]]; then
  echo "Nothing to clean (mode=$MODE)"
  exit 0
fi

echo "Cleaning mode: $MODE"
[[ $MODE == dist && $DIST_EXTRAS == yes ]] && echo "Including OVMFbin & limine (DIST_EXTRAS=yes)"

for path in "${REMOVE[@]}"; do
  if [[ $DRY_RUN == yes ]]; then
    echo "DRY-RUN: rm -rf $path"
  else
    echo "+ rm -rf $path"
    rm -rf "$path"
  fi
done

echo "Clean complete."