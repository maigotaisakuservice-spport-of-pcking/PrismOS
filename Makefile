# PrismOS — Makefile (v0.1)
#
#   make            kernel.elf と ISO を作る (build/kernel.elf, build/prismos.iso)
#   make test       シェルコアのホスト側ユニットテスト
#   make run        QEMU で ISO を起動する (要 qemu-system-x86_64)
#   make boot-test  QEMU で起動し、バナーとシェルを自動確認する
#   make clean      ビルド成果物を消す (Limine は残す)
#   make distclean  Limine のダウンロード物も消す
#
# カーネルのビルド自体はホストの gcc だけでできる (freestanding)。
# ISO 作成には Limine 8.3.2 (下記の固定タグ) と xorriso が必要。

.RECIPEPREFIX = >

CC      ?= cc
HOST_CC ?= cc

CFLAGS  := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mcmodel=kernel -m64 \
           -Wall -Wextra -O2 -I kernel -I shell
LDFLAGS := -nostdlib -no-pie -T kernel/linker.ld -z max-page-size=0x1000

# --- Limine (固定バージョン) -------------------------------------------
# 取得方法を固定する: codeload 経由で v8.3.2-binary タグの tarball を
# ダウンロードする。ブランチ (v8.x-binary) は先端が動くので使わない。
# 観測した tarball の SHA-256 (参考値):
#   abe24c940e0ffbd49f6ac72af70422ab1e99e0a9f50e010c44dfbcde27dbb609
# Limine 8 の terminal プロトコルは deprecated のため使わない。
# カーネルはシリアル (COM1) のみで入出力する。
LIMINE_VERSION := 8.3.2
LIMINE_TAG     := v$(LIMINE_VERSION)-binary
LIMINE_URL     := https://codeload.github.com/limine-bootloader/limine/tar.gz/refs/tags/$(LIMINE_TAG)

BUILD  := build
KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/prismos.iso

KERNEL_C_SRCS := kernel/main.c kernel/serial.c kernel/string.c \
                 kernel/shell.c shell/core.c
KERNEL_OBJS   := $(KERNEL_C_SRCS:%.c=$(BUILD)/obj/%.o) \
                 $(BUILD)/obj/kernel/entry.o

.PHONY: all kernel iso test run boot-test clean distclean limine

all: $(ISO)

kernel: $(KERNEL)

iso: $(ISO)

# --- カーネル ------------------------------------------------------------
$(BUILD)/obj/%.o: %.c
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/kernel/entry.o: kernel/entry.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
> @mkdir -p $(dir $@)
> $(CC) $(LDFLAGS) $(KERNEL_OBJS) -o $@
> @ls -lh $@

# --- Limine --------------------------------------------------------------
$(BUILD)/limine/limine:
> @mkdir -p $(BUILD)/limine
> @if [ ! -f $@ ]; then \
      echo "Downloading Limine $(LIMINE_VERSION) ($(LIMINE_TAG))..."; \
      curl -sSL --fail -o $(BUILD)/limine.tar.gz "$(LIMINE_URL)"; \
      tar xzf $(BUILD)/limine.tar.gz -C $(BUILD)/limine --strip-components=1; \
      rm -f $(BUILD)/limine.tar.gz; \
    fi
> $(MAKE) -C $(BUILD)/limine limine

limine: $(BUILD)/limine/limine

# --- ISO -----------------------------------------------------------------
$(ISO): $(KERNEL) $(BUILD)/limine/limine limine.conf
> @rm -rf $(BUILD)/iso_root
> @mkdir -p $(BUILD)/iso_root/boot $(BUILD)/iso_root/EFI/BOOT
> @cp $(KERNEL) $(BUILD)/iso_root/boot/prismos.elf
> @cp limine.conf $(BUILD)/iso_root/limine.conf
> @cp $(BUILD)/limine/limine-bios.sys \
       $(BUILD)/limine/limine-bios-cd.bin \
       $(BUILD)/limine/limine-uefi-cd.bin $(BUILD)/iso_root/boot/
> @cp $(BUILD)/limine/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
> @xorriso -as mkisofs -R -r -J \
      -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
      -boot-info-table -hfsplus -apm-block-size 2048 \
      --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
      --efi-boot-image --protective-msdos-label \
      $(BUILD)/iso_root -o $@
> @$(BUILD)/limine/limine bios-install $@
> @ls -lh $@

# --- テスト --------------------------------------------------------------
$(BUILD)/test_shell: tests/test_shell.c shell/core.c shell/core.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_shell.c shell/core.c

test: $(BUILD)/test_shell
> ./$(BUILD)/test_shell

# --- QEMU ----------------------------------------------------------------
# sh 経由で呼ぶ: GitHub API 経由のプッシュではスクリプトの実行権限が
# 落ちることがあるため、実行権限に依存しない。
run: $(ISO)
> sh scripts/run-qemu.sh

boot-test: $(ISO)
> python3 scripts/boot-test.py

# --- 掃除 ----------------------------------------------------------------
clean:
> @rm -rf $(BUILD)/obj $(BUILD)/kernel.elf $(BUILD)/prismos.iso \
      $(BUILD)/iso_root $(BUILD)/test_shell $(BUILD)/boot.log

distclean: clean
> @rm -rf $(BUILD)/limine $(BUILD)/limine.tar.gz
