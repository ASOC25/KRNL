#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
OVMF_DIR="$ROOT_DIR/OVMFbin"
IMG="$BUILD_DIR/disk.img"
GDB_SCRIPT="$ROOT_DIR/debug.gdb"
KERNEL_ELF="$BUILD_DIR/kernel.elf"

: "${QEMU:=qemu-system-x86_64}"
: "${GDB_BIN:=gdb}"
: "${RAM:=512}"
: "${SMP:=1}"
: "${MACHINE:=q35}"
: "${ACCEL:=}"
: "${SERIAL:=telnet:127.0.0.1:8086,server,nowait}"
: "${EXTRA_ARGS:=}"
: "${NO_GDB:=no}"

usage() {
  cat <<EOF
Usage: $0 [--img path] [--no-gdb]
EOF
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --img) IMG="$2"; shift 2;;
    --no-gdb) NO_GDB=yes; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1;;
  esac
done

CODE_FD="$OVMF_DIR/OVMF_CODE-pure-efi.fd"
VARS_FD="$OVMF_DIR/OVMF_VARS-pure-efi.fd"

for f in "$IMG" "$CODE_FD" "$VARS_FD" "$KERNEL_ELF" "$GDB_SCRIPT"; do
  [[ -f $f ]] || { echo "Required file missing: $f" >&2; exit 1; }
done

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

setsid "$QEMU" \
  -cpu qemu64 \
  -d cpu_reset \
  -no-reboot \
  -no-shutdown \
  -machine "$MACHINE" \
  "${ACCEL_ARG[@]}" \
  -m "$RAM" \
  -smp "$SMP" \
  -boot d \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE_FD" \
  -drive if=pflash,format=raw,unit=1,file="$VARS_FD" \
  -drive file="$IMG",format=raw,if=virtio \
  -serial "$SERIAL" \
  -s -S \
  $EXTRA_ARGS &

QEMU_PID=$!

echo "QEMU started (pid=$QEMU_PID) with gdb stub on :1234 and is paused waiting for 'continue'"

cleanup() {
  if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "Stopping QEMU (pid=$QEMU_PID)" >&2
    kill "$QEMU_PID"
    wait "$QEMU_PID" || true
  fi
}
trap cleanup EXIT TERM   # INT removed—handled separately below

if [[ "$NO_GDB" == yes ]]; then
  echo "NO_GDB=yes: Not launching gdb. Attach manually with:"
  echo "  $GDB_BIN -ex 'target remote :1234' $KERNEL_ELF"
  wait "$QEMU_PID"
  exit 0
fi

#
# ────────────────────────────────────────────────
#   Ignore Ctrl-C in THIS script
#   → ensures only GDB receives the SIGINT
# ────────────────────────────────────────────────
#
trap '' INT

# Launch GDB (exec replaces shell → no double handlers)
"$GDB_BIN" -q --nx --command="$GDB_SCRIPT" "$KERNEL_ELF"

echo "GDB exited. Stopping QEMU..."
cleanup

exit 0