#!/bin/sh
# PrismOS の QEMU 起動テスト。
# ISO を起動し、シリアルにコマンドを送って、バナーとシェル出力を確認する。
# 使い方: ./scripts/boot-test.sh   (終了コード 0 = 合格)
set -eu
cd "$(dirname "$0")/.."

LOG=build/boot.log
mkdir -p build
: > "$LOG"

# 起動して少し待ってからコマンドを送る。QEMU は 25 秒で止める。
{
  sleep 3; printf 'help\r';   sleep 1
  printf 'ver\r';             sleep 1
  printf 'ls\r';              sleep 1
  printf 'about\r';           sleep 1
} | timeout 25 qemu-system-x86_64 \
      -cdrom build/prismos.iso \
      -m 256M \
      -serial stdio \
      -monitor none \
      -display none \
      -no-reboot > "$LOG" 2>&1 || true

fail=0
check() {
  # check <needle> <label>
  if grep -qF "$1" "$LOG"; then
    echo "ok: $2"
  else
    echo "MISSING: $2 (expected '$1' in $LOG)"
    fail=1
  fi
}

check "PrismOS 0.1.0"      "boot banner"
check "prismos> "          "shell prompt"
check "PrismOS shell"      "help output"
check "PrismOS version 0.1.0" "ver output"
check "README.txt"         "ls output"
check "Windows 11"         "about output"

if [ "$fail" -ne 0 ]; then
  echo "--- boot log ($LOG) ---"
  cat "$LOG"
  echo "BOOT TEST FAILED"
  exit 1
fi
echo "BOOT TEST PASSED"
