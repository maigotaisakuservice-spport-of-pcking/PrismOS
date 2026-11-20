# PrismOS 設計書 (v0.1)

## コンセプト

PrismOS は「どこでもカスタムできる OS」。

- **UI は Windows 11 風**: 中央寄せタスクバー、角丸ウィンドウ、アクセントカラー
- **機能は Mac 風**: Dock、Spotlight 風検索、Mission Control 風ウィンドウ一覧
- **シェルは Linux と Windows 両対応**: `ls` も `dir` も、`cat` も `type` も動く
- **カスタマイズは設定ファイルで**: テーマ・壁紙・タスクバー位置は
  `config/theme.toml` を書き換えるだけ。OS の入れ直しは不要

名前の由来: プリズム (Prism) は 1 つの光をいろんな色に分ける。
1 つの OS が、使う人の好みに合わせて姿を変える、という意味。

## アーキテクチャ (v0.1)

```
                ┌─────────────────────────────┐
                │  Limine 8.3.2 (ブートローダ) │
                └──────────────┬──────────────┘
                               │ Limine プロトコル
                ┌──────────────▼──────────────┐
                │  kernel/                    │
                │  main.c    起動・バナー     │
                │  serial.c  COM1 入出力      │
                │  shell.c   シェルの入出力   │
                │  string.c  freestanding 用  │
                └──────────────┬──────────────┘
                               │
                ┌──────────────▼──────────────┐
                │  shell/core.c               │
                │  コマンド解析・実行 (純粋)  │
                │  カーネルとホストテスト共有 │
                └─────────────────────────────┘
```

### なぜシリアルだけなのか — Limine 8 の罠

Limine 8 では **terminal プロトコルが deprecated** になった
(`limine.h` でも `struct limine_terminal` に `LIMINE_DEPRECATED` が付く)。
Limine 7 までのチュートリアル通り terminal リクエストを使うと、
新しい Limine では動かない。回避策として v0.1 は画面出力を
一切使わず、**COM1 シリアルだけ**で入出力する。

- フレームバッファは「要求して寸法だけ報告する」に留める
  (GUI フェーズで描画コードがここに生える)
- ブートローダのメニューも `serial: yes` でシリアルに出る
- QEMU では `-serial stdio` でホストの標準入出力に繋がる

### Limine のバージョン固定

- **8.3.2** に固定。取得は codeload の `v8.3.2-binary` タグの tarball。
- `v8.x-binary` のようなブランチは先端が動くので使わない。
  (確認済み: ブランチ先端の `limine.h` は 2025 年版で、API リビジョンや
  SMP→MP の改名など v8.3.2 とは別物だった)
- タグ tarball の SHA-256 (観測値):
  `abe24c940e0ffbd49f6ac72af70422ab1e99e0a9f50e010c44dfbcde27dbb609`
- ソースから Limine をビルドする道は選ばなかった。
  BIOS ポートのビルドには `nasm` と i686 用ツールチェインが必要で、
  CI が重くなるため。バイナリタグにはビルド済みの
  `limine-bios.sys` / `limine-bios-cd.bin` / `limine-uefi-cd.bin` /
  `BOOTX64.EFI` と、ホストツール `limine` のソースが入っており、
  `make -C build/limine limine` だけで `bios-install` が使える。
- `kernel/limine.h` は同じ v8.3.2 のヘッダをベンダリングしたもの。
  カーネルのビルドはネットワーク不要。

### シェルコアの分離

`shell/core.c` は libc に依存しない純粋なロジックで、
カーネル (`-nostdlib`) とホストのユニットテストで**同じソース**を
コンパイルする。入出力 (シリアル) と副作用 (reboot / 画面クリア) は
`kernel/shell.c` にだけある。

コマンドは「正規名 + 別名」のテーブルで管理する:

| 正規名 | 別名 | 意味 |
|---|---|---|
| `ls` | `dir` | ファイル一覧 (dir は Windows 風表示) |
| `cat` | `type` | ファイル表示 |
| `clear` | `cls` | 画面クリア |
| `ver` | `version` | バージョン表示 |
| `help` | `?` | ヘルプ |
| `pwd` `cd` `echo` `about` `theme` `reboot` `exit` | — | そのまま |

v0.1 のファイルシステムは仮想 (組み込みの固定データ)。
`ls` / `cat` が見るファイルは `README.txt` `VERSION`
`theme.toml` `about.txt` の 4 つ。ディスクドライバが来たら
`shell/core.c` の仮想 FS 部分だけを差し替える。

## 将来設計 (GUI フェーズ)

### デスクトップ (v0.2)

- フレームバッファへの直接描画から始める (Limine が渡す寸法を使う)
- 描画は `kernel/` ではなく新しい `gui/` (または `desktop/`) に置く
- 最初は壁紙 1 色 + タスクバー (下・中央寄せ) + 時計だけ
- キーボードは PS/2 ドライバで割り込み入力に移行する
  (シリアルシェルはデバッグ用に残す)

### Mac 風機能 (v0.3)

- **Dock**: 画面下 (または左・右) のアイコン列。マウスオーバーで拡大。
  設定は `config/theme.toml` の `[dock]`
- **Spotlight 風検索**: `Super+Space` で開く検索窓。
  アプリ・ファイル・設定を横断検索する
- **Mission Control 風**: `Super+Tab` でウィンドウ一覧を並べる。
  ウィンドウ管理はタイルではなくフローティング (Windows / Mac 流)

### テーマエンジン (v0.4)

- `config/theme.toml` を起動時に読んでデスクトップに反映する
- アクセントカラー、壁紙、タスクバー位置・透明度、角丸を
  再起動なしで変えられる設定アプリを用意する
- テーマは TOML 1 ファイルで配布・共有できる形にする

### その先 (v1.0)

- ファイルシステム (FAT32 から始める)、実機ブート (BIOS / UEFI)
- アプリ実行形式と設定アプリ、マルチタスクの土台
- **セキュリティ**: 権限分離と署名付きテーマ/アプリ配布を前提にする
  (v0.1 の段階ではまだ何も無い — 後のフェーズで設計する)
- **初心者向け**: 設定は GUI の設定アプリから変えられることを基本とし、
  TOML の直接編集は上級者向けの近道に留める

## フェーズ計画

| フェーズ | 内容 | 状態 |
|---|---|---|
| v0.1 | 最小カーネル + Limine ブート + シリアルシェル | ✅ いまここ |
| v0.2 | フレームバッファ描画、キーボード、デスクトップの土台 | 計画 |
| v0.3 | Dock / Spotlight 風検索 / Mission Control 風 | 計画 |
| v0.4 | テーマエンジン、ファイルシステム | 計画 |
| v1.0 | 実機ブート、アプリ実行、設定アプリ | 計画 |

## ビルドとテスト

- `make` — カーネル + ISO (`build/prismos.iso`)
- `make test` — シェルコアのホストテスト (`tests/test_shell.c`)
- `make boot-test` — QEMU で ISO を起動し、バナーとシェル出力を確認
- GitHub Actions (`.github/workflows/build.yml`) が同じ 3 つを
  `ubuntu-latest` で回す。ISO は Artifact として保存される。
