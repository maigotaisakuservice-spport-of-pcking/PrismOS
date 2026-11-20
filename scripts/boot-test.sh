#!/bin/sh
# PrismOS の QEMU 起動テスト。
# ISO を起動し、シリアルにコマンドを送って、バナーとシェル出力を確認する。
# 使い方: sh scripts/boot-test.sh   (終了コード 0 = 合格)
#
# CI (TCG エミュレーション) では起動が遅いことがあるため、
# コマンドは起動を十分待ってから送り、最後のコマンドの後も
# パイプを開けたまま十分待つ。パイプが早く閉じると QEMU が
# 終了してしまい、シェルの応答が出る前にテストが終わるため。
set -eu
cd "$(dirname "$0")/.."

LOG=build/boot.log
mkdir -p build
: > "$LOG"

# 起動して十分待ってからコマンドを送る。QEMU は 40 秒で止める。
# 最後の sleep 10 が重要: 入力が終わってもすぐ EOF にせず、
# カーネルが応答を出し切るまで QEMU を生かしておく。
{
  sleep 8; printf 'help\r';   sleep 2
  printf 'ver\r';             sleep 2
  printf 'ls\r';              sleep 2
  printf 'about\r';           sleep 10
} | timeout 40 qemu-system-x86_64 \
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
# "Windows 11" はバナーにも出るため、about の判定には使わない
check "A customizable OS"  "about output"

if [ "$fail" -ne 0 ]; then
  echo "--- boot log ($LOG) ---"
  cat "$LOG"
  echo "BOOT TEST FAILED"
  exit 1
fi
echo "BOOT TEST PASSED"
