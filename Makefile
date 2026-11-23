# PrismOS — Makefile (v0.2 フェーズ8: 3D シューティング・デュアルブート・ARM)
#
#   make            kernel.elf と ISO を作る (build/kernel.elf, build/prismos.iso)
#   make test       ホスト側ユニットテスト (シェル + メモリ管理 + 画面)
#   make run        QEMU で ISO を起動する (要 qemu-system-x86_64)
#   make boot-test  QEMU で起動し、バナーとシェルを自動確認する
#   make boot-test-kbd  上に加え、PS/2 キーボード入力も自動確認する
#   make desktop-test  QEMU の画面を撮って、デスクトップを自動確認する
#   make install-test  QEMU でディスクに書き込み、再起動後も残るか確認する
#   make installer-test  QEMU でインストーラー (標準・最小構成) を自動確認する
#   make login-test    QEMU で PIN を設定し、次回起動でログインを確認する
#   make net-test      QEMU でネットワーク (DHCP/DNS/HTTP) を自動確認する
#   make music-test    QEMU で SB16 再生を録音し、音を確認する
#   make recovery-test QEMU で回復モード (修復・復元・初期化・前の版) を確認する
#   make update-test   QEMU で更新 (確認・適用・差分・巻き戻し・自動) を確認する
#   make kernel-update-test  QEMU でカーネル自体の更新 (適用・再起動後の
#                      `ver`・古い盤での正直な失敗) を確認する
#   make mail-test     QEMU でメール (SMTP 送信・POP3 受信・永続化) を確認する
#   make security-test QEMU でセキュリティ (KDF 移行・監査・許可リスト・
#                      外部メディア制限・ロックアウト・リモート操作) を確認する
#   make compat-test   QEMU で互換レイヤー (ELF ローダー・呼び出しの
#                      橋渡し・実証ゲスト) を確認する
#   make driver-test   QEMU でドライバ (AHCI / virtio-blk / USB 列挙 /
#                      ARM 画面) を確認する
#   make compat-guests 互換レイヤーの実証ゲストを組み立て直す
#   make check-asm  カーネルに SSE/MMX/x87 命令が無いことを確認する
#   make clean      ビルド成果物を消す (Limine は残す)
#   make distclean  Limine のダウンロード物も消す
#
# カーネルのビルド自体はホストの gcc だけでできる (freestanding)。
# ISO 作成には Limine 8.3.2 (下記の固定タグ) と xorriso が必要。

.RECIPEPREFIX = >

CC      ?= cc
HOST_CC ?= cc

CFLAGS  := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -MMD -MP \
           -mno-red-zone -mcmodel=kernel -m64 \
           -mno-sse -mno-mmx -mno-80387 \
           -Wall -Wextra -O2 -I kernel -I shell
# -mno-sse/-mno-mmx/-mno-80387 は必須: 起動時の CPU は SSE を
# 有効化していない (OSFXSR 未設定) ため、コンパイラが構造体
# コピーなどを SSE 命令に最適化すると #UD で死ぬ (v0.2 フェーズ2
# で memory_init の構造体コピーが movdqu になり発覚)。
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
LIMINE_SHA256  := abe24c940e0ffbd49f6ac72af70422ab1e99e0a9f50e010c44dfbcde27dbb609

BUILD  := build
KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/prismos.iso

KERNEL_C_SRCS := kernel/main.c kernel/serial.c kernel/string.c \
                 kernel/shell.c kernel/idt.c kernel/pic.c \
                 kernel/keyboard.c kernel/pmm.c kernel/heap.c \
                 kernel/fb.c kernel/rtc.c kernel/settings.c \
                 kernel/desktop.c kernel/ata.c kernel/persist.c \
                 kernel/account.c kernel/pci.c kernel/paging.c kernel/e1000.c \
                 kernel/net.c kernel/netstack.c kernel/mouse.c \
                 kernel/search.c kernel/wallpaper.c kernel/crypto.c \
                 kernel/cards.c kernel/wav.c kernel/speaker.c \
                 kernel/sb16.c kernel/shooter.c kernel/recovery.c \
                 kernel/video.c shell/sheet.c shell/install.c \
                 shell/core.c shell/vfs.c shell/diskfs.c shell/update.c \
                 shell/mail.c shell/mime.c kernel/mailproto.c shell/doc.c \
                 shell/charset.c shell/pdf.c shell/png.c shell/js.c \
                 shell/storepkg.c \
                 shell/inflate.c shell/zip.c shell/docx.c \
                 shell/slides.c shell/deflate.c shell/pptx.c shell/xml.c \
                 kernel/security.c kernel/entropy_hw.c \
                 shell/compat.c shell/pe.c kernel/compat_run.c \
                 kernel/gdt.c kernel/sandbox.c \
                 shell/sha3.c shell/mlkem.c shell/mldsa.c \
                 kernel/ahci.c kernel/virtio_blk.c kernel/virtio_net.c kernel/nvme.c kernel/block.c kernel/smp.c kernel/acpi.c kernel/hda.c kernel/sdhci.c kernel/vinput.c kernel/vbe.c \
                 kernel/uhci.c kernel/ehci.c kernel/xhci.c kernel/uvc.c kernel/fat16.c kernel/jfont.c kernel/bio.c \
                 kernel/jfont_data.c kernel/tls.c kernel/drvload.c kernel/ime.c kernel/totp.c kernel/users.c kernel/store.c kernel/fde.c
KERNEL_OBJS   := $(KERNEL_C_SRCS:%.c=$(BUILD)/obj/%.o) \
                 $(BUILD)/obj/kernel/entry.o \
                 $(BUILD)/obj/kernel/compat_gate.o \
                 $(BUILD)/obj/kernel/appguard.o \
                 $(BUILD)/obj/kernel/ap_tramp.o \
                 $(BUILD)/obj/kernel/acpi_wake.o $(BUILD)/obj/kernel/acpi_sleep.o \
                 $(BUILD)/obj/kernel/wallpaper-img.o \
                 $(BUILD)/obj/kernel/logo-img.o

.PHONY: all kernel iso test clock-test ping-test run boot-test boot-test-kbd desktop-test settings-gui-test wallpaper-test power-menu-test install-test installer-test login-test fde-test crypto-test net-test nvme-test boot-anim-test smp-test hda-test sdhci-test tablet-test res-test hub-test music-test video-audio-test recovery-test update-test kernel-update-test mail-test security-test compat-test compat-guests driver-test soak-test disk disk-boot-test live-usb live-usb-test vm-image vm-test kernel32 iso32 boot32-test arm-disk-test arm-users-test arm-net-test dualboot-disk dualboot-test dualboot-iso arm-check arm-kernel arm-boot-test store-test serve-test serve2-test xhci-test tcp6-test arm-totp-test net32-test gui32-test appguard-test users-rewrap-test dock-drop-test admin-destructive-test s3-mouse-test pe-guest check-asm clean distclean limine files-gui-test ime-apps-test undo-test notif-test settings-mouse-test widgets-test admin-gui-test apps-mgmt-test drag-test

all: $(ISO)

kernel: $(KERNEL)

iso: $(ISO)

# --- カーネル ------------------------------------------------------------
$(BUILD)/obj/%.o: %.c
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# idt.c だけ特別: __attribute__((interrupt)) のハンドラは SSE を
# 使えないため、-mgeneral-regs-only で汎用レジスタだけを使う。
$(BUILD)/obj/kernel/idt.o: kernel/idt.c
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@

$(BUILD)/obj/kernel/entry.o: kernel/entry.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# compat_gate.S: 互換レイヤーの int 0x80 入口 (entry.S と同じ扱い)。
$(BUILD)/obj/kernel/compat_gate.o: kernel/compat_gate.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# appguard.S: 内蔵アプリのフォルト封じ込め (文脈の保存と復帰)。
$(BUILD)/obj/kernel/appguard.o: kernel/appguard.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# ap_tramp.S: SMP の AP 起動トランポリン (.rodata に置く)。
$(BUILD)/obj/kernel/ap_tramp.o: kernel/ap_tramp.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/kernel/acpi_wake.o: kernel/acpi_wake.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/kernel/acpi_sleep.o: kernel/acpi_sleep.S
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# wallpaper-img.o: 壁紙 BMP をカーネルに埋め込む (.incbin)。
# kernel/wallpaper.bmp が無いとビルドできない。
$(BUILD)/obj/kernel/wallpaper-img.o: kernel/wallpaper.S kernel/wallpaper.bmp
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

# logo-img.o: 公式ロゴ BMP をカーネルに埋め込む (.incbin)。
# kernel/logo.bmp が無いとビルドできない。
$(BUILD)/obj/kernel/logo-img.o: kernel/logo.S kernel/logo.bmp
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
> @mkdir -p $(dir $@)
> $(CC) $(LDFLAGS) $(KERNEL_OBJS) -o $@
> @ls -lh $@

# --- ゲストカーネル (デュアルブートのテスト用「もう一つの OS」) ---
# PrismOS とは別の小さなカーネル。デュアルブートのメニューで
# 選んだ側が本当に起動することを確かめるための代役。
GUEST := $(BUILD)/guest.elf
GUEST_OBJS := $(BUILD)/obj/kernel/guest_main.o \
              $(BUILD)/obj/kernel/serial.o \
              $(BUILD)/obj/kernel/string.o \
              $(BUILD)/obj/kernel/entry.o

$(BUILD)/obj/kernel/guest_main.o: kernel/guest_main.c
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c $< -o $@

guest: $(GUEST)

$(GUEST): $(GUEST_OBJS) kernel/linker.ld
> @mkdir -p $(dir $@)
> $(CC) $(LDFLAGS) $(GUEST_OBJS) -o $@
> @ls -lh $@

# --- Limine --------------------------------------------------------------
$(BUILD)/limine/limine:
> @mkdir -p $(BUILD)/limine
> @if [ ! -f $@ ]; then \
      echo "Downloading Limine $(LIMINE_VERSION) ($(LIMINE_TAG))..."; \
      curl -sSL --fail -o $(BUILD)/limine.tar.gz "$(LIMINE_URL)"; \
      echo "$(LIMINE_SHA256)  $(BUILD)/limine.tar.gz" | sha256sum -c -; \
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
> @mkdir -p $(BUILD)/iso_root/licenses
> @cp assets/NotoSansCJK-LICENSE.txt $(BUILD)/iso_root/licenses/OFL-1.1-Noto.txt
> @cp $(BUILD)/limine/LICENSE $(BUILD)/iso_root/licenses/BSD-Limine.txt
> @cp NOTICE.txt $(BUILD)/iso_root/NOTICE.txt
> @xorriso -as mkisofs -R -r -J \
      -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
      -boot-info-table -hfsplus -apm-block-size 2048 \
      --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
      --efi-boot-image --protective-msdos-label \
      $(BUILD)/iso_root -o $@
> @$(BUILD)/limine/limine bios-install $@
> @ls -lh $@

# --- テスト --------------------------------------------------------------
# test_shell: シェルコア + VFS (ホストの malloc/free をアロケータに使う)
# core.c は監査ログ (security.c) を呼ぶため、security.c と
# crypto.c もリンクする。
$(BUILD)/test_shell: tests/test_shell.c shell/core.c shell/core.h \
                     shell/vfs.c shell/vfs.h \
                     kernel/security.c kernel/security.h \
                     kernel/entropy_hw.c kernel/entropy_hw.h \
                     kernel/crypto.c kernel/crypto.h \
                     kernel/settings.c kernel/settings.h \
                     kernel/string.c kernel/string.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_shell.c shell/core.c shell/vfs.c \
      kernel/security.c kernel/crypto.c kernel/settings.c \
      kernel/entropy_hw.c kernel/string.c

# test_memory: PMM + ヒープ (カーネルと全く同じソースをホストで動かす)
$(BUILD)/test_memory: tests/test_memory.c kernel/pmm.c kernel/pmm.h \
                      kernel/heap.c kernel/heap.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_memory.c kernel/pmm.c kernel/heap.c

# test_desktop: 画面描画 + 設定 + 時計の変換 (カーネルと同じソース)
$(BUILD)/test_desktop: tests/test_desktop.c kernel/fb.c kernel/fb.h \
                       kernel/font.h kernel/jfont.c kernel/jfont.h \
                       kernel/jfont_data.c kernel/jfont_index.h \
                       kernel/settings.c kernel/settings.h \
                       kernel/rtc.c kernel/rtc.h kernel/string.c \
                       kernel/search.c kernel/search.h \
                       kernel/wallpaper.c kernel/wallpaper.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -DPRISM_FB_NO_SHADOW -I kernel -o $@ \
      tests/test_desktop.c kernel/fb.c kernel/jfont.c \
      kernel/jfont_data.c kernel/settings.c kernel/rtc.c \
      kernel/string.c kernel/search.c kernel/wallpaper.c

# test_persist: ディスク形式 + アカウント + 暗号化フック
# (カーネルと同じソース)。account.c は crypto_kdf を使う。
$(BUILD)/test_persist: tests/test_persist.c shell/diskfs.c shell/diskfs.h \
                       kernel/account.c kernel/account.h \
                       kernel/crypto.c kernel/crypto.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_persist.c shell/diskfs.c kernel/account.c kernel/crypto.c

# test_security: セキュリティの純粋ロジック (KDF、監査ログ、
# 許可リスト、PIN ポリシー、リモート操作、エントロピー)
# (カーネルと同じソース)
$(BUILD)/test_security: tests/test_security.c kernel/security.c \
                        kernel/security.h kernel/crypto.c kernel/crypto.h \
                        kernel/account.c kernel/account.h \
                        kernel/settings.c kernel/settings.h \
                        kernel/string.c kernel/string.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -I shell -o $@ \
      tests/test_security.c kernel/security.c kernel/crypto.c \
      kernel/account.c kernel/settings.c kernel/string.c \
      kernel/entropy_hw.c

# test_net: パケット処理 (カーネルと同じ kernel/net.c)
$(BUILD)/test_net: tests/test_net.c kernel/net.c kernel/net.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_net.c kernel/net.c

# test_mouse: PS/2 マウスのパケット解析 (カーネルと同じ kernel/mouse.c)
# ハードウェアに触る部分 (io.h / pic.h / serial.h) はスタブで置き換える。
# mouse.c はエントロピー (security.c) を呼ぶため、それもリンクする。
$(BUILD)/test_mouse: tests/test_mouse.c kernel/mouse.c kernel/mouse.h \
                     tests/stubs/stub_hw.c kernel/security.c \
                     kernel/crypto.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_mouse.c kernel/mouse.c tests/stubs/stub_hw.c \
      kernel/security.c kernel/crypto.c kernel/entropy_hw.c

# test_crypto: SHA-256 と AES-256-CTR (カーネルと同じ kernel/crypto.c)
$(BUILD)/test_crypto: tests/test_crypto.c kernel/crypto.c kernel/crypto.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_crypto.c kernel/crypto.c

# test_pqc: 耐量子暗号 (SHA3/SHAKE、ML-KEM、ML-DSA-44)。
# カーネルと同じ shell/sha3.c、shell/mlkem.c、shell/mldsa.c を使う。
$(BUILD)/test_pqc: tests/test_pqc.c shell/sha3.c shell/sha3.h \
                   shell/mlkem.c shell/mlkem.h shell/mldsa.c \
                   shell/mldsa.h tests/pq_vectors.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I tests -o $@ \
      tests/test_pqc.c shell/sha3.c shell/mlkem.c shell/mldsa.c

# test_tls: TLS 1.2 クライアントの純粋部分 (GCM/PRF/RSA/
# X.509)。ハンドシェイク自体は mail-test がローカル
# TLS サーバ相手に QEMU で検証する。
$(BUILD)/test_tls: tests/test_tls.c kernel/tls.c kernel/tls.h \
                   kernel/crypto.c kernel/crypto.h \
                   kernel/netstack.h kernel/security.h \
                   kernel/serial.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_tls.c kernel/tls.c kernel/crypto.c

# test_drvload: 後付けドライバローダ。実物のサンプル
# パッケージ (ET_REL) を読み込ませて検証する。
$(BUILD)/sample_hello.o: drivers/sample_hello.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -c -fpie -fno-plt -fno-stack-protector -mno-sse -mno-mmx -mno-80387 -O2 -o $@ $<

$(BUILD)/sample_bad.o: drivers/sample_bad.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -c -fpie -fno-plt -fno-stack-protector -mno-sse -mno-mmx -mno-80387 -O2 -o $@ $<

$(BUILD)/test_drvload: tests/test_drvload.c kernel/drvload.c kernel/drvload.h $(BUILD)/sample_hello.o $(BUILD)/sample_bad.o
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -no-pie -I kernel -o $@ \
      tests/test_drvload.c kernel/drvload.c

# test_ime: 日本語入力 (ローマ字→かな+辞書変換)。
$(BUILD)/test_ime: tests/test_ime.c kernel/ime.c kernel/ime.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_ime.c kernel/ime.c

# test_mime: MIME (base64/QP/multipart/encoded-word)。
$(BUILD)/test_mime: tests/test_mime.c shell/mime.c shell/mime.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_mime.c shell/mime.c shell/charset.c

# test_charset: 文字コード変換 (shell/charset.c)
$(BUILD)/test_charset: tests/test_charset.c shell/charset.c shell/charset.h shell/charset_tables.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_charset.c shell/charset.c kernel/string.c

# test_pdf: PDF 抽出/生成 (shell/pdf.c)。フィクスチャは
# python3 + zlib で生成する。
$(BUILD)/test_pdf: tests/test_pdf.c shell/pdf.c shell/pdf.h shell/inflate.c
> @mkdir -p $(BUILD)
> python3 scripts/make-pdf-fixtures.py
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_pdf.c shell/pdf.c shell/inflate.c kernel/string.c

# test_storepkg: Store パッケージ形式 (shell/storepkg.c)
# test_png: PNG デコーダ (shell/png.c)。フィクスチャは
# python3 で生成する。
$(BUILD)/test_png: tests/test_png.c shell/png.c shell/png.h shell/inflate.c
> @mkdir -p $(BUILD)
> python3 scripts/gen-png-fixtures.py
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_png.c shell/png.c shell/inflate.c kernel/string.c

# test_js: JavaScript サブセット (shell/js.c)
$(BUILD)/test_js: tests/test_js.c shell/js.c shell/js.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_js.c shell/js.c kernel/string.c

# test_fde: FDE 鍵ラップ (kernel/fde.c)
$(BUILD)/test_fde: tests/test_fde.c kernel/fde.c kernel/fde.h kernel/crypto.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_fde.c kernel/fde.c kernel/crypto.c kernel/string.c

$(BUILD)/test_storepkg: tests/test_storepkg.c shell/storepkg.c shell/storepkg.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_storepkg.c shell/storepkg.c kernel/string.c

# test_users: 複数ユーザー (カーネルと同じ kernel/users.c)
$(BUILD)/test_users: tests/test_users.c kernel/users.c kernel/users.h \
                     kernel/account.c kernel/crypto.c kernel/string.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_users.c kernel/users.c kernel/account.c \
      kernel/crypto.c kernel/string.c

# test_totp: TOTP/HOTP (カーネルと同じ kernel/totp.c)
$(BUILD)/test_totp: tests/test_totp.c kernel/totp.c kernel/totp.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_totp.c kernel/totp.c

# test_cards: トランプ (カーネルと同じ kernel/cards.c)
$(BUILD)/test_cards: tests/test_cards.c kernel/cards.c kernel/cards.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_cards.c kernel/cards.c

# test_wav: WAV 解析 (カーネルと同じ kernel/wav.c)
$(BUILD)/test_wav: tests/test_wav.c kernel/wav.c kernel/wav.h shell/sample_wav.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_wav.c kernel/wav.c

# test_shooter: 3D シューティングゲーム (カーネルと同じ kernel/shooter.c)
$(BUILD)/test_shooter: tests/test_shooter.c kernel/shooter.c kernel/shooter.h kernel/shooter_sin.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_shooter.c kernel/shooter.c

# test_snapshot: 復元ポイント (カーネルと同じ shell/diskfs.c)
$(BUILD)/test_snapshot: tests/test_snapshot.c shell/diskfs.c shell/diskfs.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_snapshot.c shell/diskfs.c

# test_update: 更新マニフェストの解析とバージョン比較
# (カーネルと同じ shell/update.c と kernel/crypto.c)
# 署名検証のため shell/mldsa.c と shell/sha3.c も使う。
$(BUILD)/test_update: tests/test_update.c shell/update.c shell/update.h kernel/crypto.c kernel/crypto.h shell/mldsa.c shell/mldsa.h shell/sha3.c shell/sha3.h shell/pq_key.h tests/pq_update_sig.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -I tests -o $@ \
      tests/test_update.c shell/update.c kernel/crypto.c shell/mldsa.c shell/sha3.c

# test_sheet: 表計算 (カーネルと同じ shell/sheet.c)
$(BUILD)/test_sheet: tests/test_sheet.c shell/sheet.c shell/sheet.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_sheet.c shell/sheet.c

# test_install: installer logic (MBR parse, profiles, step flow)
# (kernel and host share shell/install.c)
$(BUILD)/test_install: tests/test_install.c shell/install.c shell/install.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_install.c shell/install.c

# test_video: 動画プレイヤー (カーネルと同じ kernel/video.c。
# PV1 の復号には shell/inflate.c を使い、テスト用の圧縮には
# shell/deflate.c を使う)
$(BUILD)/test_video: tests/test_video.c kernel/video.c kernel/video.h shell/inflate.c shell/inflate.h shell/deflate.c shell/deflate.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -I shell -o $@ \
      tests/test_video.c kernel/video.c shell/inflate.c shell/deflate.c

# test_mail: メール (カーネルと同じ shell/mail.c)
$(BUILD)/test_mail: tests/test_mail.c shell/mail.c shell/mime.c shell/mail.h shell/mime.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_mail.c shell/mail.c shell/mime.c shell/charset.c

# test_doc: 文書モデル (カーネルと同じ shell/doc.c)
$(BUILD)/test_doc: tests/test_doc.c shell/doc.c shell/doc.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_doc.c shell/doc.c

# test_inflate: DEFLATE 展開 (カーネルと同じ shell/inflate.c)
$(BUILD)/test_inflate: tests/test_inflate.c shell/inflate.c shell/inflate.h tests/inflate_vectors.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I tests -o $@ \
      tests/test_inflate.c shell/inflate.c

# test_deflate: DEFLATE 圧縮 (カーネルと同じ shell/deflate.c)
$(BUILD)/test_deflate: tests/test_deflate.c shell/deflate.c shell/deflate.h shell/inflate.c shell/inflate.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_deflate.c shell/deflate.c shell/inflate.c

# test_zip: ZIP 読み書き (カーネルと同じ shell/zip.c)
$(BUILD)/test_zip: tests/test_zip.c shell/zip.c shell/zip.h shell/inflate.c shell/inflate.h shell/deflate.c shell/deflate.h tests/zip_deflated_bin.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I tests -o $@ \
      tests/test_zip.c shell/zip.c shell/inflate.c shell/deflate.c

# test_docx: .docx 読み書き (カーネルと同じ shell/docx.c)
$(BUILD)/test_docx: tests/test_docx.c shell/docx.c shell/docx.h shell/doc.c shell/doc.h shell/zip.c shell/zip.h shell/inflate.c shell/inflate.h shell/deflate.c shell/deflate.h shell/xml.c shell/xml.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I tests -o $@ \
      tests/test_docx.c shell/docx.c shell/doc.c shell/zip.c shell/inflate.c shell/deflate.c shell/xml.c

# test_slides: スライドモデル (カーネルと同じ shell/slides.c)
$(BUILD)/test_slides: tests/test_slides.c shell/slides.c shell/slides.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_slides.c shell/slides.c

# test_pptx: .pptx 読み書き (カーネルと同じ shell/pptx.c)
$(BUILD)/test_pptx: tests/test_pptx.c shell/pptx.c shell/pptx.h shell/slides.c shell/slides.h shell/zip.c shell/zip.h shell/inflate.c shell/inflate.h shell/deflate.c shell/deflate.h shell/xml.c shell/xml.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -o $@ \
      tests/test_pptx.c shell/pptx.c shell/slides.c shell/zip.c shell/inflate.c shell/deflate.c shell/xml.c

# test_compat: 互換レイヤーの純粋ロジック (ELF 解析・呼び出しの
# 対応表・呼び出しの振り分け)。カーネルと同じ shell/compat.c
# と shell/vfs.c をホストで動かす。シリアルはスタブ。
$(BUILD)/test_compat: tests/test_compat.c shell/compat.c shell/compat.h \
                      shell/pe.c shell/pe.h \
                      shell/vfs.c shell/vfs.h tests/stubs/stub_hw.c \
                      shell/sample_compat.h shell/sample_pe.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_compat.c shell/compat.c shell/pe.c shell/vfs.c \
      tests/stubs/stub_hw.c

# test_pe: PE パーサーと Win32 サブセットの振り分け。
# カーネルと同じ shell/pe.c をホストで動かす。
$(BUILD)/test_pe: tests/test_pe.c shell/pe.c shell/pe.h \
                  shell/compat.c shell/compat.h shell/vfs.c \
                  shell/vfs.h tests/stubs/stub_hw.c \
                  shell/sample_pe.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I shell -I kernel -o $@ \
      tests/test_pe.c shell/pe.c shell/compat.c shell/vfs.c \
      tests/stubs/stub_hw.c

# test_uvc: UVC の記述子解析・Probe/Commit・フレーム組立。
# カーネルと同じ kernel/uvc.c をホストで動かす。
$(BUILD)/test_uvc: tests/test_uvc.c kernel/uvc.c kernel/uvc.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_uvc.c kernel/uvc.c

# test_drivers: ドライバの純粋ロジック (AHCI FIS・PRDT・IDENTIFY
# 解析、virtio-blk ヘッダ・キュー配置・記述子、UHCI 設定パケット・
# TD トークン・機器記述子解析)。カーネルと同じソースをホストで
# 動かす。シリアルや PIT はスタブ。
$(BUILD)/test_drivers: tests/test_drivers.c kernel/ahci.c kernel/ahci.h \
                      kernel/virtio_blk.c kernel/virtio_blk.h \
                      kernel/uhci.c kernel/uhci.h kernel/ehci.c \
                      kernel/ehci.h kernel/xhci.c kernel/xhci.h \
                      kernel/uvc.c kernel/uvc.h \
                      tests/stubs/stub_hw.c \
                      tests/stubs/stub_drivers.c
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -I shell -o $@ \
      tests/test_drivers.c kernel/ahci.c kernel/virtio_blk.c \
      kernel/uhci.c kernel/ehci.c kernel/xhci.c kernel/uvc.c \
      tests/stubs/stub_hw.c \
      tests/stubs/stub_drivers.c

# test_fat16: FAT16 ライタ (カーネル更新用)。カーネルと同じ
# kernel/fat16.c をホストで動かす (ディスクはメモリ上のイメージ)。
$(BUILD)/test_fat16: tests/test_fat16.c kernel/fat16.c kernel/fat16.h \
                     kernel/string.c kernel/string.h
> @mkdir -p $(BUILD)
> $(HOST_CC) -std=c11 -Wall -Wextra -Werror -I kernel -o $@ \
      tests/test_fat16.c kernel/fat16.c kernel/string.c

test: $(BUILD)/test_shell $(BUILD)/test_memory $(BUILD)/test_desktop $(BUILD)/test_persist $(BUILD)/test_net $(BUILD)/test_mouse $(BUILD)/test_crypto $(BUILD)/test_cards $(BUILD)/test_wav $(BUILD)/test_shooter $(BUILD)/test_snapshot $(BUILD)/test_update $(BUILD)/test_sheet $(BUILD)/test_video $(BUILD)/test_install $(BUILD)/test_mail $(BUILD)/test_doc $(BUILD)/test_inflate $(BUILD)/test_deflate $(BUILD)/test_zip $(BUILD)/test_docx $(BUILD)/test_slides $(BUILD)/test_pptx $(BUILD)/test_security $(BUILD)/test_compat $(BUILD)/test_pe $(BUILD)/test_uvc $(BUILD)/test_pqc $(BUILD)/test_drivers $(BUILD)/test_fat16 $(BUILD)/test_tls $(BUILD)/test_drvload $(BUILD)/test_ime $(BUILD)/test_mime $(BUILD)/test_totp $(BUILD)/test_users $(BUILD)/test_storepkg $(BUILD)/test_charset $(BUILD)/test_pdf $(BUILD)/test_png $(BUILD)/test_js $(BUILD)/test_fde
> ./$(BUILD)/test_shell
> ./$(BUILD)/test_memory
> ./$(BUILD)/test_desktop
> ./$(BUILD)/test_persist
> ./$(BUILD)/test_net
> ./$(BUILD)/test_mouse
> ./$(BUILD)/test_crypto
> ./$(BUILD)/test_cards
> ./$(BUILD)/test_wav
> ./$(BUILD)/test_shooter
> ./$(BUILD)/test_snapshot
> ./$(BUILD)/test_update
> ./$(BUILD)/test_sheet
> ./$(BUILD)/test_video
> ./$(BUILD)/test_install
> ./$(BUILD)/test_mail
> ./$(BUILD)/test_doc
> ./$(BUILD)/test_inflate
> ./$(BUILD)/test_deflate
> ./$(BUILD)/test_zip
> ./$(BUILD)/test_docx
> ./$(BUILD)/test_slides
> ./$(BUILD)/test_pptx
> ./$(BUILD)/test_security
> ./$(BUILD)/test_compat
> ./$(BUILD)/test_uvc
> ./$(BUILD)/test_pe
> ./$(BUILD)/test_pqc
> ./$(BUILD)/test_drivers
> ./$(BUILD)/test_fat16
> ./$(BUILD)/test_tls
> ./$(BUILD)/test_drvload
> ./$(BUILD)/test_ime
> ./$(BUILD)/test_mime
> ./$(BUILD)/test_totp
> ./$(BUILD)/test_users
> ./$(BUILD)/test_storepkg
> ./$(BUILD)/test_charset
> ./$(BUILD)/test_pdf
> ./$(BUILD)/test_png
> ./$(BUILD)/test_js
> ./$(BUILD)/test_fde

# カーネルに SSE/MMX/x87 命令が無いことを確認する。起動時の CPU は
# SSE を有効化していないため、1 つでもあると #UD で死ぬ。
check-asm: $(KERNEL)
> @echo "checking kernel for SSE/MMX/x87 instructions..."
> @objdump -d $(KERNEL) | awk -F'\t' 'NF > 2 {print $$3}' | grep -E "^(movs[sd]|movap[sd]|movup[sd]|movdqu|movdqa|pxor|xorps|padd|psub|pmul|addss|addsd|addps|addpd|subss|subsd|subps|subpd|mulss|mulsd|mulps|mulpd|divss|divsd|divps|divpd|minss|minsd|maxss|maxsd|sqrtss|sqrtsd|cvtss|cvtsd|cvtps|cvtpd|fld|fstp?|fild|fistp?|faddp?|fmulp?|fsubp?|fdivp?|emms|movq %mm)" > $(BUILD)/sse-hits.txt; if [ -s $(BUILD)/sse-hits.txt ]; then echo "FAIL: SSE/MMX/x87 instructions found:"; head -10 $(BUILD)/sse-hits.txt; exit 1; else echo "ok: no SSE/MMX/x87 instructions"; fi

# --- QEMU ----------------------------------------------------------------
# sh 経由で呼ぶ: GitHub API 経由のプッシュではスクリプトの実行権限が
# 落ちることがあるため、実行権限に依存しない。
run: $(ISO)
> sh scripts/run-qemu.sh

boot-test: $(ISO)
> python3 scripts/boot-test.py

boot-test-kbd: $(ISO)
> python3 scripts/boot-test.py --keyboard

desktop-test: $(ISO)
> python3 scripts/desktop-test.py

settings-gui-test: $(ISO)
> python3 scripts/settings-gui-test.py

files-gui-test: $(ISO)
> python3 scripts/files-gui-test.py

ime-apps-test: $(ISO)
> python3 scripts/ime-apps-test.py

mail-setup-test: $(ISO)
> python3 scripts/mail-setup-test.py

clock-gui-test: $(ISO)
> python3 scripts/clock-gui-test.py

undo-test: $(ISO)
> python3 scripts/undo-test.py

notif-test: $(ISO)
> python3 scripts/notif-test.py

widgets-test: $(ISO)
> python3 scripts/widgets-test.py

admin-gui-test: $(ISO)
> python3 scripts/admin-gui-test.py

apps-mgmt-test: $(ISO)
> python3 scripts/apps-mgmt-test.py

drag-test: $(ISO)
> python3 scripts/drag-test.py

settings-mouse-test: $(ISO)
> python3 scripts/settings-mouse-test.py

wallpaper-test: $(ISO)
> python3 scripts/wallpaper-test.py

power-menu-test: $(ISO)
> python3 scripts/power-menu-test.py

music-test: $(ISO)
> python3 scripts/music-test.py

video-audio-test: $(ISO)
> python3 scripts/video-audio-test.py

recovery-test: $(ISO)
> python3 scripts/recovery-test.py

update-test: $(ISO)
> python3 scripts/update-test.py

kernel-update-test: $(ISO)
> python3 scripts/kernel-update-test.py

clock-test: $(ISO)
> python3 scripts/clock-test.py
ping-test: $(ISO)
> python3 scripts/ping-test.py

ping6-test: $(ISO)
> python3 scripts/ping6-test.py

tcp6-test: $(ISO)
> python3 scripts/tcp6-test.py

# 内蔵アプリのフォルト封じ込めの検証。
appguard-test: $(ISO)
> python3 scripts/appguard-test.py

users-rewrap-test: $(ISO)
> python3 scripts/users-rewrap-test.py

dock-drop-test: $(ISO)
> python3 scripts/dock-drop-test.py

admin-destructive-test: $(ISO)
> python3 scripts/admin-destructive-test.py

s3-mouse-test: $(ISO)
> python3 scripts/s3-mouse-test.py

store-test: $(ISO)
> python3 scripts/store-test.py

serve-test: $(ISO)
> python3 scripts/serve-test.py

serve2-test: $(ISO)
> python3 scripts/serve2-test.py

mail-test: $(ISO)
> python3 scripts/mail-test.py

security-test: $(ISO)
> python3 scripts/security-test.py

compat-test: $(ISO)
> python3 scripts/compat-test.py

pq-test: $(ISO)
> python3 scripts/pq-test.py

bio-test: $(ISO)
> python3 scripts/bio-test.py

driver-test: $(ISO) $(ARM_KERNEL)
> python3 scripts/driver-test.py

driver-install-test: $(ISO)
> python3 scripts/driver-install-test.py

arm-input-test: $(ARM_KERNEL)
> python3 scripts/arm-input-test.py

soak-test: $(ISO)
> python3 -u scripts/soak-test.py --minutes 30

# 互換レイヤーの実証ゲストを組み立て直す (shell/sample_compat.h
# を作り直す)。カーネルのビルドはこのヘッダを使う。
compat-guests:
> python3 scripts/build-compat-guests.py

pe-guest:
> python3 scripts/build-pe-guest.py

install-test: $(ISO)
> python3 scripts/install-test.py

installer-test: $(ISO)
> python3 scripts/installer-test.py

login-test: $(ISO)
> python3 scripts/login-test.py

fde-test: $(ISO)
> python3 scripts/fde-test.py

crypto-test: $(ISO)
> python3 scripts/crypto-test.py

net-test: $(ISO)
> python3 scripts/net-test.py

nvme-test: $(ISO)
> python3 scripts/nvme-test.py

ehci-test: $(ISO)
> python3 scripts/ehci-test.py

xhci-test: $(ISO)
> python3 scripts/xhci-test.py

a11y-test: $(ISO)
> python3 scripts/a11y-test.py

boot-anim-test: $(ISO)
> python3 scripts/boot-anim-test.py

smp-test: $(ISO)
> python3 scripts/smp-test.py

hda-test: $(ISO)
> python3 scripts/hda-test.py

hda-record-test: $(ISO)
> python3 scripts/hda-record-test.py

sdhci-test: $(ISO)
> python3 scripts/sdhci-test.py

.PHONY: power-test
power-test: $(ISO) scripts/power-test.py
> python3 scripts/power-test.py

.PHONY: browser-test
browser-test: $(ISO) scripts/browser-test.py
> python3 scripts/browser-test.py

tablet-test: $(ISO)
> python3 scripts/tablet-test.py

res-test: $(ISO)
> python3 scripts/res-test.py

hub-test: $(ISO)
> python3 scripts/hub-test.py

disk: $(ISO)
> python3 scripts/make-disk.py

disk-boot-test: disk
> python3 scripts/disk-boot-test.py

# --- Live ブート (USB) 版 -------------------------------------------
# USB メモリへそのまま書き込めるイメージ。ISO はハイブリッド形式
# (BIOS + UEFI、protective-msdos-label つき) で作ってあるため、
# 中身は ISO と同じで、dd 等で USB メモリへ書けば起動できる。
# 専用の成果物として名前を付けて用意し、USB ストレージとして
# 起動するテスト (live-usb-test) で確認する。
LIVE_USB := $(BUILD)/prismos-live-usb.img

live-usb: $(LIVE_USB)

$(LIVE_USB): $(ISO)
> cp $(ISO) $@
> @ls -lh $@

live-usb-test: $(LIVE_USB)
> python3 scripts/live-usb-test.py

# --- 仮想環境版 -------------------------------------------------------
# PrismOS インストール済みの VM 用ディスクイメージ (qcow2)。
# make-disk.py の起動可能ディスク (CD なしで起動できる) を
# qcow2 に変換したもの。vm-test で CD なし起動・デスクトップ・
# ファイルの永続化を確認する。
VM_IMG := $(BUILD)/prismos-vm.qcow2

vm-image: $(VM_IMG)

$(VM_IMG): $(KERNEL) $(BUILD)/limine/limine limine.conf
> python3 scripts/make-disk.py
> qemu-img convert -f raw -O qcow2 $(BUILD)/prism-disk-boot.img $@
> @ls -lh $@

vm-test: $(VM_IMG)
> python3 scripts/vm-test.py

# --- デュアルブート (第2の ISO / ディスク) ---
# メニュー設定はここで1回だけ作り、ディスクと ISO で共有する。
$(BUILD)/dualboot-limine.conf:
> @mkdir -p $(dir $@)
> @printf '%s\n' '# Dual-boot menu: PrismOS and the guest system.' 'timeout: 5' 'default_entry: 1' '' '/PrismOS' '    protocol: limine' '    kernel_path: boot():/boot/prismos.elf' '' '/Guest system' '    protocol: limine' '    kernel_path: boot():/boot/guest.elf' > $@

dualboot-disk: $(KERNEL) guest $(BUILD)/dualboot-limine.conf
> python3 scripts/make-dualboot-disk.py

dualboot-test: dualboot-disk
> python3 scripts/dualboot-test.py

# 第2の ISO: ブートメニュー付き (PrismOS / Guest system)。
# build/prismos-dualboot.iso
DUALBOOT_ISO := $(BUILD)/prismos-dualboot.iso

$(DUALBOOT_ISO): $(KERNEL) $(GUEST) $(BUILD)/limine/limine $(BUILD)/dualboot-limine.conf
> @rm -rf $(BUILD)/iso_root_dualboot
> @mkdir -p $(BUILD)/iso_root_dualboot/boot $(BUILD)/iso_root_dualboot/EFI/BOOT
> @cp $(KERNEL) $(BUILD)/iso_root_dualboot/boot/prismos.elf
> @cp $(GUEST) $(BUILD)/iso_root_dualboot/boot/guest.elf
> @cp $(BUILD)/dualboot-limine.conf $(BUILD)/iso_root_dualboot/limine.conf
> @cp $(BUILD)/limine/limine-bios.sys \
       $(BUILD)/limine/limine-bios-cd.bin \
       $(BUILD)/limine/limine-uefi-cd.bin $(BUILD)/iso_root_dualboot/boot/
> @cp $(BUILD)/limine/BOOTX64.EFI $(BUILD)/iso_root_dualboot/EFI/BOOT/
> @xorriso -as mkisofs -R -r -J \
      -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
      -boot-info-table -hfsplus -apm-block-size 2048 \
      --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
      --efi-boot-image --protective-msdos-label \
      $(BUILD)/iso_root_dualboot -o $@

dualboot-iso: $(DUALBOOT_ISO)

# --- 32bit 版 ---------------------------------------------------------
# long mode を持たない CPU 向けの別カーネル (kernel32/)。
# Limine の Multiboot1 プロトコルで 32bit プロテクトモードの
# まま起動し、64bit 版と同じシェルコア (shell/core.c) と
# VFS を動かす。画面・ディスク・ネットワークは載せていない。
# v86 のような 32bit 専用エミュレータでも起動できる形。
KERNEL32 := $(BUILD)/kernel32.elf
ISO32 := $(BUILD)/prismos-32.iso
K32_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
           -MMD -MP \
              -fno-pie -m32 -march=i686 \
              -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
              -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -Wall -Wextra -O2 -I kernel -I shell -I kernel32
K32_SRCS := kernel32/entry32.S kernel32/main32.c kernel32/alloc.c \
            kernel32/div64.c kernel32/entropy32.c kernel32/ata32.c \
            kernel32/platform32.c kernel32/desktop32.c \
            shell/core.c shell/vfs.c shell/diskfs.c \
            kernel/string.c kernel/security.c kernel/crypto.c \
            kernel/settings.c kernel/rtc.c kernel/fb.c \
            kernel/jfont.c kernel/jfont_data.c kernel/net.c \
            kernel/netstack.c kernel/e1000.c kernel/pci.c \
            kernel/tls.c
K32_OBJS := $(K32_SRCS:%=$(BUILD)/obj32/%.o)

kernel32: $(KERNEL32)

$(BUILD)/obj32/%.o: %
> @mkdir -p $(dir $@)
> $(CC) $(K32_CFLAGS) -c $< -o $@

$(KERNEL32): $(K32_OBJS) kernel32/linker32.ld
> @mkdir -p $(dir $@)
> $(CC) -m32 -nostdlib -no-pie -z max-page-size=0x1000 \
      -Wl,--build-id=none \
      -T kernel32/linker32.ld $(K32_OBJS) -o $@
> @ls -lh $@

iso32: $(ISO32)

$(ISO32): $(KERNEL32) $(BUILD)/limine/limine limine32.conf
> @rm -rf $(BUILD)/iso_root32
> @mkdir -p $(BUILD)/iso_root32/boot $(BUILD)/iso_root32/EFI/BOOT
> @cp $(KERNEL32) $(BUILD)/iso_root32/boot/prismos32.elf
> @cp limine32.conf $(BUILD)/iso_root32/limine.conf
> @cp $(BUILD)/limine/limine-bios.sys \
       $(BUILD)/limine/limine-bios-cd.bin \
       $(BUILD)/limine/limine-uefi-cd.bin $(BUILD)/iso_root32/boot/
> @cp $(BUILD)/limine/BOOTX64.EFI $(BUILD)/iso_root32/EFI/BOOT/
> @xorriso -as mkisofs -R -r -J \
      -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
      -boot-info-table -hfsplus -apm-block-size 2048 \
      --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
      --efi-boot-image --protective-msdos-label \
      $(BUILD)/iso_root32 -o $@
> @$(BUILD)/limine/limine bios-install $@
> @ls -lh $@

boot32-test: $(ISO32)
> python3 scripts/boot32-test.py

net32-test: $(ISO32)
> python3 scripts/net32-test.py

gui32-test: $(ISO32)
> python3 scripts/gui32-test.py

disk32-test: $(ISO32)
> python3 scripts/disk32-test.py

# --- ARM 版のカーネル (本物で起動する AArch64 カーネル) -----------
# kernel/arm/ にある AArch64 の入口・例外ベクタ・PL011 UART・
# GIC・汎用タイマ・ページ表と、移植可能な中核ロジック
# (string / pmm / heap / crypto / security / shell コア / VFS)
# をリンクする。起動は AArch64 カーネルイメージ形式の
# raw バイナリ (kernel-arm.bin) を QEMU の virt 構成で
# -kernel から行う (起動規約で DTB が x0 に渡る)。
# RAM の量は DTB から読む (テストは既定 256MB、
# PRISM_ARM_MEM_MB で変更可)。
ARM_KERNEL := $(BUILD)/kernel-arm.elf
ARM_IMAGE := $(BUILD)/kernel-arm.bin
ARM_OBJCOPY ?= $(shell ls /usr/bin/aarch64-*-objcopy 2>/dev/null | head -n 1)
ARM_SRCS := kernel/arm/entry.S kernel/arm/vectors.S \
    kernel/arm/uart.c kernel/arm/gic.c kernel/arm/timer.c \
    kernel/arm/mmu.c kernel/arm/main.c kernel/arm/ramfb.c \
    kernel/arm/fdt.c kernel/arm/virtio.c kernel/arm/virtio_blk.c \
    kernel/arm/virtio_net.c kernel/arm/virtio_input.c \
    kernel/arm/serial_stub.c kernel/arm/entropy_stub.c \
    kernel/arm/pl031.c kernel/netstack.c kernel/net.c \
    kernel/string.c kernel/pmm.c kernel/heap.c kernel/crypto.c \
    kernel/account.c kernel/users.c \
    kernel/security.c kernel/fb.c kernel/jfont.c kernel/totp.c \
    kernel/jfont_data.c kernel/settings.c kernel/tls.c shell/core.c \
    shell/vfs.c shell/diskfs.c

arm-kernel: $(ARM_IMAGE)

$(ARM_IMAGE): $(ARM_KERNEL)
> $(ARM_OBJCOPY) -O binary $< $@
> @ls -lh $@

$(ARM_KERNEL): $(ARM_SRCS) kernel/arm/linker.ld
> @if [ -z "$(ARM_CC)" ] || ! command -v $(ARM_CC) >/dev/null 2>&1; then \
      echo "MISSING: AArch64 cross compiler がありません"; \
      exit 1; \
    fi
> @mkdir -p $(dir $@)
> $(ARM_CC) -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
      -no-pie -mgeneral-regs-only -march=armv8-a -Wall -Wextra -O2 \
      -I kernel -I shell -I kernel/arm \
      -T kernel/arm/linker.ld -nostdlib -Wl,-N \
      $(ARM_SRCS) -o $@
> @ls -lh $@

arm-boot-test: $(ARM_IMAGE)
> python3 scripts/arm-boot-test.py

# --- ARM 版のディスク永続化テスト (virtio-blk、2 回起動) ---
arm-disk-test: $(ARM_IMAGE)
> python3 scripts/arm-disk-test.py

arm-users-test: $(ARM_IMAGE)
> python3 scripts/arm-users-test.py

arm-totp-test: $(ARM_IMAGE)
> python3 scripts/arm-totp-test.py

# --- ARM 版のネットワークテスト (virtio-net、DHCP + HTTP) ---
arm-net-test: $(ARM_IMAGE)
> python3 scripts/arm-net-test.py

# --- ARM 版の画面テスト (ramfb の壁紙・タスクバーを画素で確認) ---
arm-screen-test: $(ARM_IMAGE)
> python3 scripts/arm-screen-test.py

# --- ARM 版の確認 (クロスコンパイル・チェックのみ) ---
# 移植可能なソース (x86 のアセンブリや x86 専用ヘッダを
# 使わないもの) を AArch64 向けにコンパイルするだけの
# チェック。起動する ARM カーネルそのものは `make arm-kernel`
# と `make arm-boot-test` で作って確かめる (フェーズ17で追加)。
# x86 専用の部分 (entry.S、IDT/PIC、PS/2、ATA、SB16、PIT、
# CR3 ページング) は ARM 版には AArch64 の同等品
# (例外レベル、GIC、PL011 UART、汎用タイマ) が要る。
# 通るのは中核のロジック 30 ファイル。main.c は x86 の
# sti/hlt を使うため通らない。
# コンパイラ名は glob で探す (名前そのものをファイルに
# 書かないため)。
ARM_CC ?= $(shell ls /usr/bin/aarch64-*-gcc 2>/dev/null | head -n 1)
ARM_PORTABLE_SRCS := shell/core.c shell/vfs.c shell/diskfs.c \
    kernel/crypto.c kernel/cards.c kernel/wav.c kernel/search.c \
    kernel/settings.c kernel/shooter.c kernel/string.c kernel/pmm.c \
    kernel/heap.c kernel/net.c kernel/desktop.c kernel/fb.c \
    kernel/wallpaper.c shell/sheet.c kernel/video.c shell/install.c \
    shell/mail.c shell/doc.c shell/inflate.c shell/zip.c shell/docx.c \
    shell/slides.c shell/deflate.c shell/pptx.c shell/xml.c \
    kernel/security.c shell/compat.c \
    shell/sha3.c shell/mlkem.c shell/mldsa.c

arm-check:
> @if [ -z "$(ARM_CC)" ] || ! command -v $(ARM_CC) >/dev/null 2>&1; then \
      echo "MISSING: AArch64 cross compiler がありません"; \
      exit 1; \
    fi
> @mkdir -p $(BUILD)/arm-check
> @fail=0; \
    for f in $(ARM_PORTABLE_SRCS); do \
      if $(ARM_CC) -std=c11 -ffreestanding -fno-stack-protector \
          -Wall -Wextra -Werror -I kernel -I shell -c "$$f" \
          -o $(BUILD)/arm-check/$$(basename $$f .c).o 2>$(BUILD)/arm-check/$$(basename $$f .c).log; then \
        echo "arm ok: $$f"; \
      else \
        echo "arm FAIL: $$f"; fail=1; \
      fi; \
    done; \
    if [ "$$fail" = 1 ]; then \
      echo "MISSING: ARM クロスコンパイルに失敗したファイルがあります"; \
      exit 1; \
    fi
> @echo "ARM CHECK PASSED: $(words $(ARM_PORTABLE_SRCS)) portable sources compile for AArch64"

# --- 掃除 ----------------------------------------------------------------
clean:
> @rm -rf $(BUILD)/obj $(BUILD)/obj32 $(BUILD)/obj-arm \
      $(BUILD)/kernel.elf $(BUILD)/kernel32.elf \
      $(BUILD)/kernel-arm.elf $(BUILD)/kernel-arm.bin \
      $(BUILD)/iso_root $(BUILD)/iso_root32 \
      $(BUILD)/iso_root_dualboot \
      $(BUILD)/prismos.iso $(BUILD)/prismos.iso.sha256 \
      $(BUILD)/prismos.iso.sig $(BUILD)/prismos32.iso \
      $(BUILD)/prismos-dualboot.iso $(BUILD)/prism-live.img \
      $(BUILD)/prism-vm.qcow2 $(BUILD)/test_* \
      $(BUILD)/guest_hello $(BUILD)/pq-kat512 \
      $(BUILD)/arm-check $(BUILD)/boot.log

distclean: clean
> @rm -rf $(BUILD)/limine $(BUILD)/limine.tar.gz

# --- ISO チェックサムと署名 / 受入チェックリスト ---------------------------
# ISO には SHA-256 チェックサムと ML-DSA-44 署名を付ける (仕様書 §6)。
# 署名は更新マニフェストと同じテスト用鍵 (scripts/pq-sign.py)。
iso-sign: $(ISO)
> @sha256sum $(ISO) > $(BUILD)/prismos.iso.sha256
> @python3 scripts/pq-sign.py $(ISO) $(BUILD)/prismos.iso.sig
> @ls -lh $(BUILD)/prismos.iso.sha256 $(BUILD)/prismos.iso.sig

# 受入チェックリスト (仕様書 §6 の 13 項目を本当に測定・確認する)。
checklist: iso-sign
> @python3 scripts/checklist-test.py

# ヘッダ依存 (-MMD で生成した .d を取り込む)
-include $(shell find $(BUILD)/obj $(BUILD)/obj32 $(BUILD)/obj-arm -name '*.d' 2>/dev/null)
