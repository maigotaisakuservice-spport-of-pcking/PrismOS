#!/bin/sh
# PrismOS を QEMU で起動する (手動操作用)。
# シリアルはホストの標準入出力に繋がる。Ctrl-A X で終了。
set -e
cd "$(dirname "$0")/.."
exec qemu-system-x86_64 \
  -cdrom build/prismos.iso \
  -m 256M \
  -serial stdio \
  -monitor none \
  -display none \
  -no-reboot
