#!/usr/bin/env bash
set -euo pipefail

# Simple helper to open gdb and connect to a QEMU gdb stub.
# Usage: ./scripts/connect-to-debug.sh [host] [port]
# Defaults: host=127.0.0.1, port=1234

HOST=${1:-127.0.0.1}
PORT=${2:-1234}
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
OVMF_DIR="$ROOT_DIR/OVMFbin"
IMG="$BUILD_DIR/disk.img"
GDB_SCRIPT="$ROOT_DIR/debug.gdb"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
if ! command -v gdb >/dev/null 2>&1; then
	echo "Error: 'gdb' is required but not found in PATH." >&2
	exit 1
fi

echo "Starting gdb and connecting to $HOST:$PORT"

# Make it so after gdb ends, it tries again with: gdb -q --nx --command="$GDB_SCRIPT" "$KERNEL_ELF".

#Do it in a loop
while true; do
	gdb -q --nx --command="$GDB_SCRIPT" "$KERNEL_ELF"
	echo "gdb session ended. Reconnecting in 2 seconds..."
	sleep 2
done