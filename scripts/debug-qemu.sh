#!/usr/bin/env bash

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
OVMF_DIR="$ROOT_DIR/OVMFbin"
IMG="$BUILD_DIR/disk.img"
GDB_SCRIPT="$ROOT_DIR/debug.gdb"
KERNEL_ELF="$BUILD_DIR/kernel.elf"

: "${QEMU:=qemu-system-x86_64}"
: "${RAM:=512M}"
: "${MACHINE:=q35}"
: "${ACCEL:=}"
: "${SMP:=1}"

CODE_FD="$OVMF_DIR/OVMF_CODE-pure-efi.fd"
VARS_FD="$OVMF_DIR/OVMF_VARS-pure-efi.fd"

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

#
# ────────────────────────────────────────────────
#   Launch QEMU in its own process group (setsid)
#   → prevents Ctrl-C from killing QEMU
# ────────────────────────────────────────────────
#

"$QEMU" \
  -machine "$MACHINE" \
  -d cpu_reset \
  -no-reboot \
  -no-shutdown \
  -cpu qemu64 \
  -smp "$SMP" \
  -m "$RAM" \
  -boot d \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE_FD" \
  -drive if=pflash,format=raw,unit=1,file="$VARS_FD" \
  -drive file="$IMG",format=raw,if=virtio \
  -serial "stdio" \
  -s -S \