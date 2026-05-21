#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
OVMF_DIR="$ROOT_DIR/env"
IMG="$BUILD_DIR/disk.img"

: "${QEMU:=qemu-system-x86_64}"
: "${RAM:=2048}"               # RAM in MB
: "${SMP:=1}"                  # vCPUs
: "${MACHINE:=q35}"            # Machine type
: "${ACCEL:=}"                 # e.g. kvm (auto-detected if empty)
: "${SERIAL:=stdio}"           # Serial backend
: "${DEBUG_CONSOLE:=no}"       # yes to enable isa-debugcon
: "${GDB:=no}"                 # yes to wait for gdb on :1234
: "${EXTRA_ARGS:=}"            # Additional raw QEMU args

usage() {
  cat <<EOF
Usage: $0 [options]

Environment overrides (export VAR=value):
  QEMU            QEMU binary (default: qemu-system-x86_64)
  RAM             Memory in MB (default: 512)
  SMP             vCPU count (default: 1)
  MACHINE         Machine type (default: q35)
  ACCEL           Accel hint: kvm|tcg (default: auto detect)
  SERIAL          Serial backend (default: stdio)
  DEBUG_CONSOLE   yes to enable isa-debugcon @ 0x402 (default: no)
  GDB             yes to start -s -S (wait for gdb) (default: no)
  EXTRA_ARGS      Extra args appended verbatim

Options:
  --img PATH      Use custom disk image (default: build/disk.img)
  -h, --help      Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --img) IMG="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1;;
  esac
done

CODE_FD="$OVMF_DIR/OVMF_CODE-pure-efi.fd"
VARS_FD="$OVMF_DIR/OVMF_VARS-pure-efi.fd"

if [[ ! -f "$IMG" ]]; then
  echo "Disk image not found: $IMG (build it with: make image)" >&2
  exit 1
fi
if [[ ! -f "$CODE_FD" || ! -f "$VARS_FD" ]]; then
  echo "OVMF firmware files missing in $OVMF_DIR" >&2
  exit 1
fi

ACCEL_ARG=()
if [[ -z "$ACCEL" ]]; then
  if [[ $(uname -s) == Linux && -r /dev/kvm ]]; then
    ACCEL_ARG+=( -accel kvm )
  else
    ACCEL_ARG+=( -accel tcg )
  fi
else
  ACCEL_ARG+=( -accel "$ACCEL" )
fi

DBG_ARGS=()
if [[ "$GDB" == yes ]]; then
  DBG_ARGS+=( -s -S )
fi
if [[ "$DEBUG_CONSOLE" == yes ]]; then
  DBG_ARGS+=( -device isa-debugcon,iobase=0x402,chardev=dbg -chardev stdio,id=dbg,signal=off )
fi

set -x
exec "$QEMU" \
  -machine "$MACHINE" \
  "${ACCEL_ARG[@]}" \
  -m "$RAM" \
  -smp "$SMP" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE_FD" \
  -drive if=pflash,format=raw,unit=1,file="$VARS_FD" \
  -drive file="$IMG",format=raw,if=virtio \
  -serial "$SERIAL" \
  -display gtk \
  "${DBG_ARGS[@]}" \
  $EXTRA_ARGS