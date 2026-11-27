# PrismOS 設計書 (v0.1)

## コンセプト

PrismOS は「どこでもカスタムできる OS」。

- **UI は中央寄せのモダンな見た目**: 中央寄せタスクバー、角丸ウィンドウ、アクセントカラー
- **機能は Dock と横断検索**: Dock、横断検索、ウィンドウ一覧
- **シェルは 2 つのコマンド体系に対応**: `ls` も `dir` も、`cat` も `type` も動く
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
                │  idt.c     IDT・例外ハンドラ│
                │  pic.c     PIC リマップ     │
                │  keyboard.c PS/2 キーボード │
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
| `ls` | `dir` | ファイル一覧 (dir は表形式の表示) |
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

### デスクトップ (v0.2 フェーズ3 で実装済み)

- フレームバッファへの直接描画 (Limine が渡す寸法を使う)。
  `kernel/fb.c` が 32/24 ビット描画、`kernel/font.h` が自前の
  5x7 ビットマップフォント、`kernel/desktop.c` が画面全体を描く
- 壁紙は Canva で作った画像 (1920x1080 PNG を 1280x800 の
  24 ビット BMP に変換し、カーネルに組み込み) を表示する。
  `kernel/wallpaper.c` が BMP を読み、設定で切り替えると
  従来の描画 (グラデーション + プリズムの三角形と 3 色の
  光の筋) に戻せる。画像ファイルそのものを起動時に読む
  仕組みはまだ無い (v0.2 フェーズ6)
- タスクバー (下・中央寄せ、CMOS 時計で実時刻を表示) + Dock
  (ターミナル / ファイル / 設定 / ブラウザ / メモ / カレンダー /
  写真 / 音楽 / カード)。F1〜F7・F9〜F11 でアプリを切り替える
- 画面のアプリは 9 つとも本物: ターミナルはシリアルシェルと
  同じ入出力、ファイルは VFS の一覧・開く・削除、設定は
  アクセント色・タスクバー位置・明暗・壁紙を変えて `settings.txt`
  に保存する。ブラウザは HTML を描画し（見出し・リンク・色・BMP 画像、CSS サブセット、HTTPS は証明書ピン留め TOFU）、
  メモは `note.txt` を編集して保存し、カレンダーは実日付を
  表示する (v0.2 フェーズ5)。写真は VFS の BMP ファイル
  (24 ビット、16384 バイトまで) を開いて表示し、音楽は VFS の
  WAV ファイル (8 ビット・モノラル、16384 バイトまで) を SB16
  で鳴らし、カードはシャッフルしたデッキでブラックジャックを
  遊べる (v0.2 フェーズ7)。インストール済みならディスクに
  書き戻され、再起動後も残る (v0.2 フェーズ4)。未インストールなら
  RAM 上だけで、再起動では消える
- キーボードは PS/2 ドライバで割り込み入力 (v0.2 フェーズ1)。
  シリアルとキーボードの両方からシェルに入力できる。
  シリアルシェルはデバッグ用に残す
- マウスは PS/2 ドライバで割り込み入力 (v0.2 フェーズ6)。
  画面にカーソルが出て、Dock のボタンを押してアプリを
  切り替えられる。ファイル一覧では行を押して開ける
- まだ無いもの: 複数ウィンドウの並べ替え、
  `config/theme.toml` の起動時読み込み
  (設定の既定値は TOML と同じ値だが、読むのは `settings.txt`)

### Dock と横断検索 (v0.2 フェーズ6 で実装済み)

- **Dock**: 実装済み (v0.2 フェーズ3)。画面下のボタン列で、
  選んだアプリがアクセント色になる。マウスで押して切り替え
  られる (v0.2 フェーズ6)。マウスオーバー拡大は未対応
- **横断検索**: F8 で開く検索窓 (`kernel/search.c`)。
  アプリ・ファイル・設定を横断検索し、Enter で開く
  (v0.2 フェーズ6)。検索結果の一覧がウィンドウ一覧の
  代わりになる。タイル / フローティングのウィンドウ管理は
  未実装 (アプリは 1 つずつ全画面表示)

### 写真・音楽・カード (v0.2 フェーズ7)

- **写真** (F9): VFS の `.bmp` ファイル (24 ビット無圧縮)
  を一覧し、Enter で開いて画面に表示する。`kernel/wallpaper.c`
  の BMP 解析と `fb_draw_image_rgb24` を再利用する。1 ファイル
  16384 バイトまでのため、表示できるのは小さな画像
  (例: 32x32 の 24 ビット BMP) だけ。起動時の組み込み写真は
  `assets/prism-small.bmp` (`prism.bmp` として VFS に入る)
- **音楽** (F10): VFS の `.wav` ファイル (8 ビット・モノラル
  PCM) を SB16 (`kernel/sb16.c`、ISA DMA チャンネル 1) で
  鳴らす。`play <file>` シェルコマンドでも鳴らせる。録音して
  確かめた (`scripts/music-test.py`: 0.39 秒、主周波数 520 Hz)。
  PC スピーカーの `beep` コマンドもあるが、この QEMU (8.2)
  には PC スピーカーの音声出力が無いため録音では確かめられない
- **カード** (F11): ブラックジャック。`kernel/cards.c` の純粋
  ロジック (52 枚のデッキ、シャッフル、手札の合計、ディーラーの
  17 で止まるルール) で遊ぶ。n = 新しいゲーム、h = ヒット、
  s = スタンド。勝敗数を数える。賭けは無い
- 文書・プレゼンテーション (オフィススイート): フェーズ13で実装済み
  (Ctrl+D / Ctrl+P、.docx/.pptx の読み書き)
- 未実装: トランプのブラックジャック以外の
  ゲーム。写真は BMP だけ (PNG/JPEG の読み込みは無い)

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

### 永続化とインストーラー (v0.2 フェーズ4)

- ATA ディスクドライバ (`kernel/ata.c`): PIO モードで
  プライマリ・マスタのディスクを 1 セクタずつ読み書きする。
  書き込み後は FLUSH CACHE を送る。ディスクが無くても
  起動は続く (その場合はインストールも永続化もできない)。
- ディスク形式 (`shell/diskfs.c`): セクタ 0 が MBR
  (署名 0x55AA、パーティション 1 個、種別 0x83)、セクタ 1 が
  スーパーブロック ("PRISMFS2")、セクタ 2-9 がファイル表
  (64 件)、セクタ 10 からデータ (1 ファイル 32 セクタの固定枠)。
  ハードウェアに触らないためホストテストで同じソースを動かせる。
- インストーラー: シェルの `install yes` がディスクを
  初期化して今のファイルを全部書き込む。再起動後に同じ
  ディスクを付けて CD から起動すると、ファイルがそのまま
  残っている。削除したファイルも消えたままになる。
- ディスク単体での起動 (v0.2 フェーズ6): `make disk` が
  32MB のディスクを作る。先頭に FAT16 の起動用
  パーティション (Limine のブートローダとカーネル入り) と、
  その後ろにデータ用パーティション (PRISMFS2) を置く。
  CD 無しでそのディスクだけから起動できることを
  `make disk-boot-test` で確認している。データ用
  パーティションに書き込むとき、MBR と起動用パーティション
  は触らない。
- ログイン: `passwd` で PIN を設定すると、".account"
  ファイルに SHA-256 の反復ハッシュ (10000 回、16 バイト
  ソルト) で保存される (v2 形式。古い v1 形式のアカウントは
  ログイン時に自動で移行する)。次回起動では PIN を
  試せる回数に上限があり、間違え続けるとロックアウトする。
  (FNV-1a はスナップショットのチェックサム等、暗号用途
  以外でのみ使う。)
- 暗号化 (v0.2 フェーズ6): ログインで確かめた PIN から
  鍵を作る (SHA-256(PIN || ソルト)、`kernel/crypto.c`)。
  ".account" 以外のファイルの中身は AES-256-CTR で暗号化
  してディスクに書く。IV はファイルごとに新しく作り、
  ファイル表に置く。起動時は暗号文のまま読み込み、
  ログイン後に鍵で復号する。PIN を設定していない間は
  ファイルは平文のまま。IV の乱数はハードウェア乱数では
  なく、時計とカウンタを混ぜたもの。ディスク全体の暗号化
  (ブート時の解除) は未実装。`make crypto-test` で、
  ディスクの生データに平文が無いことと、再起動後の
  ログインで読めることを確認している。

## フェーズ計画

| フェーズ | 内容 | 状態 |
|---|---|---|
| v0.1 | 最小カーネル + Limine ブート + シリアルシェル | ✅ 完了 |
| v0.2 | キーボード入力、メモリ管理、ファイルシステムの土台 | ✅ 完了 |
| v0.2 フェーズ4 | ディスクへの永続化、インストーラー、ログインの土台 | ✅ 完了 |
| v0.2 フェーズ5 | ネットワーク (e1000 + DHCP + DNS + TCP + HTTP)、画面アプリ (ブラウザ・メモ・カレンダー) | ✅ 完了 (外への通信は環境の都合で未確認) |
| v0.2 フェーズ6 | ディスク単体起動、PS/2 マウス、壁紙画像、横断検索、ファイル暗号化 | ✅ 完了 |
| v0.2 フェーズ7 | バージョン 0.2.0、写真アプリ (BMP)、音楽アプリ (SB16 で WAV 再生)、カードゲーム (ブラックジャック) | ✅ 完了 (動画・メール・オフィス・3D ゲームは未実装) |
| v0.2 フェーズ8 | 3D シューティングゲーム (レイキャスティング、F12)、デュアルブート ISO とデュアルブート用ディスク、ARM 版の確認 (中核のロジック 16 ファイル) | ✅ 完了 (ARM で起動するカーネルは未実装) |
| v0.2 フェーズ9 | 回復モード (修復・復元・初期化・前の版からの修復)、更新システム (差分更新、自動ロールバック、ベータチャンネル) | ✅ 完了 (カーネル自体の更新は未実装) |
| v0.2 フェーズ10 | 表計算アプリ (10 列 × 20 行、数式・SUM/AVG/MIN/MAX、`sheet.csv` に保存、Ctrl+O)、動画アプリ (`vid-*.bmp` 12 フレームを 4 fps で再生、Ctrl+V)、ARM 版の確認を 18 ファイルに拡大 | ✅ 完了 (メールは未実装、動画は 32x32 の BMP 連番のみ、表計算は 10 列 × 20 行まで) |
| v0.2 フェーズ11 | 画面つきインストーラーアプリ (Ctrl+I、パーティション画面、標準・最小構成、進捗表示、ファイル検証、完了後の再起動)、ARM 版の確認を 19 ファイルに拡大 | ✅ 完了 |
| v0.2 フェーズ12 | メールアプリ (SMTP 送信・POP3 受信、Ctrl+M、受信メールは `mail-NNNN.txt` でディスクに保存し再起動後も残る)、ARM 版の確認を 20 ファイルに拡大 | ✅ 完了 (TLS 未対応でパスワードとメールは平文、最大 28 通まで、フォルダ・添付なし。VFS 64 ファイル化 (最終フェーズ Wave 1) で 14 通から 28 通に増えた) |
| v0.2 フェーズ13 | 文書アプリ (見出し・箇条書き・番号付き・太字の編集、`doc.txt` に保存、Ctrl+D、.docx の書き出し・読み込み)、プレゼンテーションアプリ (8 枚までのスライド編集、`slides.txt` に保存、Ctrl+P でスライドショー、.pptx の書き出し・読み込み)、ARM 版の確認を 28 ファイルに拡大 | ✅ 完了 (当時は .docx の番号付き段落は他アプリ製のものを箇条書きとして読み、.pptx の書き出しは 4096 バイト制限のため短いスライド 3 枚までだった。最終フェーズ Wave 2 で番号付き段落の読み取りと 8 枚までの書き出しに対応済み。スライドの削除は画面からできない、レイアウトファイルは出さない) |
| v0.2 フェーズ14 | セキュリティ強化 (PIN ハッシュを SHA-256 反復 10000 回・16 バイトソルトの KDF に置き換え、v1 アカウントはログイン時に v2 へ自動移行、RDRAND とキー・マウス・時刻を混ぜた乱数、外部メディア制限、アプリ許可リスト、ハッシュチェーンつき監査ログ、`security` / `audit` コマンド、PIN ポリシーとロックアウト、リモートロック／ワイプ)、ARM 版の確認を 29 ファイルに拡大 | ✅ 完了 (指紋・顔認証はハードウェア依存で未実装、外部メディア制限はディスクへの読み書きの制限で USB 機器そのものの検出・遮断ではない、リモートワイプは利用者ファイルの削除まで、監査ログは平文で最大 20 件、外部メディア制限中は監査ログがディスクに残らない) |
| v0.2 フェーズ15 | 動画コーデック (自前 PV1 形式: 160x120 RGB565、8 フレームごとのキーフレーム、フレーム間差分、DEFLATE 圧縮、1 フレーム 4096 バイト以内、ホスト側エンコーダ `scripts/encode-video.py`、BMP 連番の読み込みも引き続き対応、再生・停止・シーク・コマ送り・最初から再生)、ARM 版の確認は 29 ファイルのまま | ✅ 完了 (当時は音声なし、最大 160x120、1 ファイル 4096 バイトの上限があるため長い動画はフレーム数が限られた。最終フェーズ Wave 3 で音声トラック付き・320x240・24 フレームまで対応済み) |
| v0.2 フェーズ16 | 互換レイヤー (静的ELF実行ファイルのローダー `shell/compat.h/.c`、`int $0x80` ゲート `kernel/compat_gate.S` とランナー `kernel/compat_run.h/.c`、呼び出し橋渡し exit/write/read/open/close/fstat/brk/getpid、未対応は -38 (ENOSYS) を返す、実証ゲスト4本 `compat/hello.s` / `calc.s` / `cat.s` / `unsup.s` とホスト側ビルド `scripts/build-compat-guests.py`、シェル `run` / `compat info` / `compat list` / `compat calls`、ファイルマネージャーで .elf を開くと実行、`make compat-test` と desktop-test step 21)、ARM 版の確認を 30 ファイルに拡大 | ✅ 完了 (動的リンク・グラフィック・ネットワーク・GUIを使うアプリは未対応、ゲストに隔離はなく壊れたポインタを触るとカーネルごと止まる、PE 形式や他 OS の exe / .app / .dmg の完全対応は未対応) |
| v0.2 フェーズ17 | ARM カーネルが起動する (AArch64 の入口 `kernel/arm/entry.S`（EL3/EL2/EL1 から EL1 へ落とす）、PL011 UART `kernel/arm/uart.h/.c`、GICv2 `kernel/arm/gic.h/.c`、汎用タイマ `kernel/arm/timer.h/.c`（10ms 間隔の割り込み、起動時に 50 刻み待って割り込みが本当に来ることを確認）、MMU `kernel/arm/mmu.h/.c`（恒等写像、L0/L1 の 1GB ブロック）、例外ベクタ `kernel/arm/vectors.S`、メイン `kernel/arm/main.c`（x86 版と同じシェルコアで help / ver / ls / cat / echo / rm / touch / cd / pwd / theme / reboot が動く、ARM だけの `mem` / `ticks` コマンド、PSCI リセットで再起動）、`make arm-kernel` と `make arm-boot-test`（15 項目 PASSED）) | ✅ 完了 (デスクトップ・ファイルシステム・ネットワーク・ディスク・画面アプリはまだ ARM では動かない。タイマの PPI 番号は DTB が示す 30（非セキュア物理タイマ）で、27 は EL2 用のため届かない。QEMU は EL1 から起動する) |
| v0.2 フェーズ18 | 耐量子暗号 (SHA3/SHAKE、ML-KEM-512/768/1024、ML-DSA-44 を純粋Cで実装。ML-DSA は Python の cryptography ライブラリと PQClean 参照実装の両方と相互検証済み（鍵生成の公開鍵が一致、ライブラリの署名を C の検証が受け、C の署名をライブラリと PQClean が受ける。改ざんした署名・メッセージは拒否）。ML-KEM-768/1024 は公開ベクトルと一致し、512 は一貫性と改ざん検出を確認。更新マニフェストの署名検証を `update check` / `update apply` / 自動セキュリティ更新 / 回復モードの前バージョン修復に組み込み、署名のないもの・改ざんされたものは拒否する。ホスト側の署名ツール `scripts/pq-sign.py`、秘密鍵は .gitignore。`make test` 2230 件 PASS、ARM 版の確認は 33 ファイルのまま) | ✅ 完了 (ML-KEM は鍵交換の実運用（相手との鍵共有の手順）までは未実装で、鍵生成・カプセル化・デカプセル化の部品まで。ML-DSA の鍵生成・署名はホスト側ツールのみで、カーネル側は検証だけを行う。ML-KEM-512 は公開ベクトルと照合していない) |
| v0.2 フェーズ19 | ドライバーと安定化 (AHCI SATA `kernel/ahci.h/.c`（q35 の AHCI コントローラ、LBA48 読み書き DMA、PRDTL はコマンドヘッダ DW0 の 31:16 ビット）、virtio-blk `kernel/virtio_blk.h/.c`（PCI 1AF4:1001、3 記述子のキュー、完了は開始時の used 位置と比べて待つ）、ブロック層 `kernel/block.h/.c`（ATA PIO → AHCI → virtio-blk の順で選ぶ）、USB UHCI `kernel/uhci.h/.c`（制御転送、最大パケットで分割、交互トグル、状態段階はデータ段階と逆向き、1 TD 1 フレームで進むため PIT で最大 200ms 待つ）、シェル `disk` / `usb` コマンド、`tests/test_drivers.c` 55 件、`scripts/driver-test.py`（AHCI・virtio-blk・USB を QEMU で確認）、30 分ソークテスト `scripts/soak-test.py`（`make soak-test`、1 分おきに `mem` で空き量を記録）、ARM ramfb `kernel/arm/ramfb.h/.c`（fw_cfg の読みと選択は動くが設定の書き込みが画面に反映されず未完了だった。最終フェーズ Wave 5 で fw_cfg DMA 書き込みに直して画面が出るようになった）、ARM 版の確認は 33 ファイルのまま | ✅ 完了 (AHCI・virtio-blk・USB 列挙は QEMU で検証済み。ARM の画面は当時は未完了でシリアルだけのままだったが、最終フェーズ Wave 5 で修正済み。USB は列挙までで、USB キーボード／マウスからの入力と USB メモリの読み書きは未実装) |
| 最終フェーズ Wave 1 | VFS を 32 ファイル×4096 バイトから 64 ファイル×16384 バイトへ引き上げ、ディスク形式を PRISMFS2 (64 件×32 セクタ、スナップショットは使用中ファイルの分だけ写す) へ移行、カーネルヒープを 2MB へ拡大 | ✅ 完了 (`make test` 2349 件 PASS、QEMU テスト全 PASSED) |
| 最終フェーズ Wave 2 | .pptx 書き出しの 8 枚対応 (短いスライド 8 枚で 7027 バイト、16384 バイトのファイル上限に収まる。4096 バイトの出力先には収まらず -1 を返す。python-pptx で 8 枚のタイトルと箇条書きが読めることを確認) と、.docx 番号付き段落の読み取り (`shell/docx.c` が word/numbering.xml を解析し、numId → abstractNumId → 段落定義の対応を取り、直接の w:numPr と様式だけの段落 (ListNumber など) を "1. " の形で読む。10 進・英字・ローマ数字、入れ子の段落、開始番号に対応。番号づけが分からない段落は箇条書きに落とす) | ✅ 完了 (`make test` 2359 件 PASS。`tests/test_pptx.c` 74 件、`tests/test_docx.c` 61 件。python-docx 製の実物ファイルで "1. first step" 〜 "4. fourth step" が番号付きで読めることを確認。制限: 100 以上の番号も読める (doc.c の判定を 1〜3 桁に広げて対応済み)。styles.xml が 16384 バイトを超える場合は読まず、numbering.xml の pStyle の結びつけだけで様式を引く) |
| 最終フェーズ Wave 3 | 動画への音声追加と解像度の引き上げ (PV1 に音声トラックを追加: 映像データのあとに 8 ビット符号なし PCM 8000 Hz を 1 フレーム分 2000 サンプル = 0.25 秒付ける後方互換の形式。音声のない古い PV1 もそのまま読める。サンプル動画は C5 523.25 Hz の正弦波。再生は表示中のフレームの音声を SB16 で鳴らし、音声の長さがフレーム時間そのものなので次のフレームへは音声終了後に進む。解像度は 160x120 から 320x240 へ (1 フレーム約 2.4KB で 16384 バイト上限に十分収まる。表示は 384x288 のまま)。新規テスト `scripts/video-audio-test.py` (`make video-audio-test`) が QEMU の SB16 録音で音声を検証) | ✅ 完了 (`make test` 2385 件 PASS。`tests/test_video.c` 90 件。`make video-audio-test` PASSED: 4 秒再生でシリアルに 13 行の "video: audio frame"、録音 3.24 秒・RMS 15944・ピーク 23039・主周波数 520 Hz。`make desktop-test` 21 ステップ PASSED、`make music-test` PASSED、`make boot-test` PASSED。制限: 音は毎フレーム同じ C5 の正弦波。BMP 連番フォールバックの動画には音声なし。フレーム数は Wave 3 直後に 24 フレームへ拡大（組み込み vid-01〜24.pv1、合計約 58KB。`make test` 2373 件 PASS、desktop-test・video-audio-test・crypto-test PASSED。hello.elf は Files 一覧の 32 行目に移ったため desktop-test ステップ 21 を修正） |
| 最終フェーズ Wave 4 | カーネル自体の更新 (本物。制限リスト項目 5)。最小限の FAT16 ライタ `kernel/fat16.h/.c` (MBR の先頭パーティションを探して BPB を読む、FAT16 のみで FAT12/FAT32 は -2 で断る、空きクラスタの割り当て、鎖の書き込み、2 つの FAT コピー両方の更新、ディレクトリエントリの作成・更新、`/boot/prismos.elf` と `/boot/prismos.new` のクラスタ番号＋サイズを同じセクタへの 1 回の書き込みで入れ替える `fat16_swap_entries`。FAT セクタへの書き出しはまとめて行う: 割り当て中はキャッシュだけを汚し、FAT セクタの切り替え時と完了・中止・削除・入れ替えの時にだけ両コピーへ書き出す)。マニフェストに `kernel <sha256>` 行を追加 (`shell/update.h/.c`、ML-DSA 署名でカバー)。`update apply` は HTTP からストリーミングで取得しながら `/boot/prismos.new` へ書き込む (`kernel/netstack.h/.c` の `net_fetch_http_stream`、ヘッダ解析と Content-Length、本体をコールバックへ。`kernel/crypto.h/.c` にストリーミング SHA-256 を追加)。書き込みながら SHA-256 を進め、一致したらディレクトリエントリの入れ替えで `/boot/prismos.elf` と置き換える。失敗時は `.new` を消して旧カーネルが残り、ファイル側も復元ポイントへ巻き戻す。起動パーティション P1 を 8MB から 16MB へ広げ (`scripts/make-disk.py` / `make-dualboot-disk.py`、DATA_START 18432 → 34816、ディスクは 32MB のまま、P2 は 30720 セクタ)、旧式 8MB 盤では「boot partition is not FAT16」と正直に失敗する。`ver` / `about` が `PRISM_VERSION` を読むよう修正 (`shell/core.c`、従来の "0.2.0" ハードコードを除去)。新規テスト `scripts/kernel-update-test.py` (`make kernel-update-test`、QEMU 6 ブート) | ✅ 完了 (`make test` 2477 件 PASS。`tests/test_fat16.c` 79 件 (FAT 書き出しのまとめを数える検査と生イメージの鎖の検査を含む)、`tests/test_crypto.c` 21 件、`tests/test_update.c` 54 件。`make kernel-update-test` PASSED: 0.2.0 → 0.3.0 のカーネル更新→再起動→`ver` が 0.3.0 を表示、旧式 8MB 盤は理由つきで失敗しファイルも 0.2.0 へロールバック、20MB カーネルは途中で空き切れとなり 0.3.0 カーネルが残る。`make boot-test` / `disk-boot-test` / `update-test` / `desktop-test` / `installer-test` PASSED、`make check-asm` 0、商標語 grep 0。制限: インストーラーの whole-disk (項目 13) は未完了 — ブロック層がディスク 1 台のみで、CD 起動時に Limine の MBR 起動コードとカーネルファイルのバイト列をカーネル内で入手する手段がない。`install whole` は追加していない。IDE (ATA PIO) では 1 セクタごとに 256 回のポート入出力が必要で、TCG エミュレーション下では 4.6MB の書き込みに 180 秒の期限でも 874KB しか進まないため、テストのディスクは AHCI 接続 (DMA) にした。FAT 書き出しのまとめで IDE でも約 3 倍速くなったが TCG 下では依然遅い (実機・KVM では PIO でも速い見込みだが未検証)。バージョンは 0.2.0 のまま (更新で入る 0.3.0 はテスト用)) |
| 最終フェーズ Wave 5 | 仕様書 §6 チェックリストの検証機構と修正、公式ロゴ、ブラウザの URL 入力、シェルの cp/mv、ARM ramfb の修正 (制限リスト項目 14・監査の指摘対応)。`scripts/checklist-test.py` + `make checklist` / `make iso-sign` (13 項目を本物の測定で確認し、合格／不合格／未検証を分ける。ISO の SHA-256 と ML-DSA 署名を作る)。設定アプリに Dock の表示／非表示 ('k'、設定 `dock=show|hide`、`kernel/settings.h/.c`、`kernel/desktop.c`)、`help` に日本語の説明を追加し、出力を 2048 バイトから 4096 バイトに広げた (`kernel/shell.c` の OUT_CAP。従来は末尾が切れていた)。インストーラーの「ディスク全体」は起動可能なディスクを作れないため正直に断るよう修正 (`kernel/desktop.c:3325`、処理に渡していなかった `inst_target` を受けて拒否。`scripts/installer-test.py` に空ディスクでの拒否確認を追加)。公式ロゴは明るい版に決定 (Canva DAHbf4AwTkU、`assets/logo-light.png`、README と仕様書に掲載)。起動画面にロゴを表示 (`kernel/logo.bmp` 640px 幅の 24 ビット BMP を埋め込み、`kernel/main.c` が白い画面の中央に 1.5 秒表示)。ブラウザにアドレス入力欄を追加 (`kernel/desktop.c`、初期値 example.com、入力して Enter でそのホストを取得。`scripts/desktop-test.py` に example.org 取得の手順 5b を追加)。シェルに `cp` / `mv` を追加 (`shell/core.c`、`tests/test_shell.c` に 12 件)。ARM ramfb の修正: 設定の書き込みはデータレジスタではできない (QEMU の fw_cfg_write は QEMU 2.4 以降は空関数) ため fw_cfg DMA 書き込みに変更 (`kernel/arm/ramfb.c`、OSDev の Ramfb と実例 arm-v8a-virt-ramfb、QEMU の hw/display/ramfb.c と hw/nvram/fw_cfg.c を読んで特定。DMA 記述子は volatile が要る — コンパイラがポーリングを最適化で消した実例あり)。`make test` 2489 件 PASS、`make boot-test` / `desktop-test` / `arm-boot-test` / `arm-screen-test` (1024x768、壁紙・プリズム・タスクバーを画素で確認) / `installer-test` / `kernel-update-test` PASSED。VM 置換で apt ツールと Python パッケージが消えたため、キャッシュの .deb と pip で復旧 (QEMU 8.2.2、xorriso 1.5.6、mtools 4.0.43、aarch64-gcc 13.3.0、cryptography) | ✅ 完了 (チェックリスト 13 項目のうち不合格だった 2・3・4・6・13 は、直せる部分 (Dock の表示切替・日本語ヘルプ・ブラウザの URL 入力) を直し、直せない部分 (サンドボックス、ショートカットの GUI 編集、メール TLS/IMAP、USB 入力・メモリ、ARM デスクトップ、動的リンク、指紋・顔認証、32bit 版) は未完了として残す。`make checklist` の結果は `build/checklist-result.txt` と最終報告を参照) |
| 最終フェーズ Wave 6 | チェックリスト不合格の原因調査と修正、4 つの上限引き上げ、テストハーネスの実バグ修正。`scripts/security-test.py` の `patch_v1_account` を PRISMFS2 に合わせた (表は 64 件×64 バイト、データは 10 セクタ目から 32 セクタ／ファイル。Wave 1 の形式変更でテスト側の 32 件・4096 バイトの決め打ちが古くなっていた。製品の不具合ではなくテスト側の陳腐化)。項目 9 の USB ブートは QEMU の起動失敗が原因 (`-device usb-storage` に `usb-bus` がない。`-usb` でコントローラを有効化し、USB ストレージからの起動を確認。QEMU の stderr を残すよう改善)。項目 10 はテストハーネスの `read_ppm` の実バグが原因 (`split(b"\n", 4)` がバイナリ中の 0x0A でも分割して先頭の画素を欠落させ、画像全体をずらす。`split(b"\n", 3)` に修正。正しく解析するとタイトルバーは白い画素 101 個。設定画面の開いた確認をタイトルバーと見出しの 2 か所にした。desktop-test・installer-test・arm-screen-test の解析は正しかった)。項目 5 は自動キー操作だけでは初心者の 5 分を確認できないため合格から未検証へ変更。項目 13 の静的リストの古い記述（ramfb 未解決、whole-disk テストなし）を現状に合わせた（installer-test を実行して判定する）。上限引き上げ 4 件：メール本文 1024→4096 バイト（`kernel/desktop.c` の mail_body。GUI で 1100 文字の本文を入力し、ローカル SMTP サーバで 1100 文字ちょうど受信して確認）、シェル行 256→512 文字（`shell/core.h` の PRISM_SHELL_LINE_CAP と `kernel/shell.c` の LINE_CAP。300 文字のトークンが切れずに出ることをホストテストで確認し、512 バイトでの切り詰めも確認）、横断検索 16→32 件（`kernel/desktop.c` の SEARCH_MAX_RESULTS。F8 で空の検索語を入れ 27 行の結果が出ることを画面で確認）、アクセント色 4 色目に緑 0x4CC26B（`kernel/settings.h/.c`、`kernel/desktop.c` の '4' キーと色名表。設定画面で '4' を押してタイトル下線が緑になり、settings.txt に accent=#4CC26B と保存されることを確認）。`make test` 2497 件 PASS | ✅ 完了 (チェックリスト再実行の結果は `build/checklist-result.txt` と最終報告を参照。直せなかった項目 4・6・13 の残り（サンドボックス、ショートカットの GUI 編集）と、実機・Actions が要る未検証項目は未完了のまま残す) |
| 5種ビルドフェーズ + サンドボックス (2026-11-22) | 5 種のビルド成果物を正式化し、互換ゲストのサンドボックスを実装。(1) Live ブート (USB) 版: `make live-usb` (ハイブリッド ISO をそのまま書き込める `build/prismos-live-usb.img`) + `make live-usb-test` (QEMU で USB ストレージとして起動し、バナー・シェル・デスクトップ画面を確認) PASSED。(2) 仮想環境版: `make vm-image` (インストール済み qcow2) + `make vm-test` (CD なしで起動、再起動をまたいでファイルが残る) PASSED。(3) 32bit 版: 新規 `kernel32/`。Limine の Multiboot1 プロトコルで 32bit プロテクトモードのまま起動 (long mode なし)。64bit 版と同じシェルコアが動作。`make iso32` / `make boot32-test` (qemu-system-i386) PASSED。デスクトップ (画面) は未搭載でシリアルシェルのみ (正直な未完了)。(4) ARM フル版: DTB パーサ (`kernel/arm/fdt.c`) で RAM 量を読み PMM の 16MB 上限を解消 (約 255MB)。【同日追加修正】MMU の恒等写像を DTB の RAM 末尾まで拡張 (1GB ブロックを必要な分だけ、上限 64GB)。その過程で、QEMU が ELF カーネルに起動規約を適用せず DTB が渡っていなかった根本問題を特定し (従来は 256MB フォールバックで隠れていた)、AArch64 イメージヘッダ付き raw バイナリ (`build/kernel-arm.bin`) で起動する方式に変更して DTB 取得を本物にした (DTB 領域は PMM から除外)。`-m 4096` (4GB) で PMM 1,048,093 ページと boot/disk/screen テスト PASSED を確認、256MB 回帰も全通過。virtio-mmio レガシートランスポート (`kernel/arm/virtio.c`。QEMU virt の実配置は 0x200 刻み 32 トランスポート、GuestPageSize=0x028 を書かないと QueuePFN が壊れることを QEMU の公開ソースで特定) + ブロックドライバで diskfs 永続化 (`disk`/`dformat`/`dsync`/`dload`/`dls`、起動時自動ロード、`make arm-disk-test` PASSED)。virtio-net + netstack 無改造移植で DHCP/HTTP (`make arm-net-test`)。デスクトップアプリの対話操作は入力デバイス (virtio-input) が無く未完了 (ramfb 表示は動作済み)。(5) サンドボックス: 自前 GDT + TSS (`kernel/gdt.c`、ユーザーモード用セグメント追加)、ゲスト専用ページテーブル (`kernel/sandbox.c`、カーネル半分は管理者専用のまま共有)、リング 3 実行 + int $0x80 ゲート DPL=3、例外時はゲストだけ終了、PIT IRQ0 ウォッチドッグ (約 5 秒) で暴走ゲストも停止。検証ゲスト evil.elf (カーネルメモリ読み→フォルトで終了) と loop.elf (無限ループ→時間制限で終了) を追加し `make compat-test` PASSED (既存 4 ゲストも継続動作)。内蔵デスクトップアプリ自体の隔離は未完了 (アーキテクチャ変更のため対象外)。(6) Actions: 5 種をビルド+起動テストする `.github/workflows/build-variants.yml` をローカルに用意 (プッシュはしない。ワークフローはブラウザ経由でのみ追加可能)。(7) 並行網羅チェックの指摘 B1〜B6 を修正: zip.c の 32bit 境界チェック巻き戻し、mlkem.c の定数時間比較、docx.c/pptx.c のタグ範囲外読み、update.c の hex 解析 1 バイト過読み、テスト側 2 件。回帰テスト追加で `make test` 2506 件 PASS、ASan/UBSan でも該当テスト全通過 | ✅ 完了 (未完了として残す: 32bit デスクトップ、ARM デスクトップ対話操作、Actions の実走 (テストブランチへのプッシュは別ステップ)、チェックリスト項目 6 はこの実装で合格見込みだが `make checklist` の再実行で確定する) |
| 残りの制限フェーズ (2026-11-22) | 残っていた制限の解消と全層精査の反映。(1) ショートカット GUI 編集 (設定の2ページ目で取り込み・交換・初期化・保存、デスクトップで実効)。(2) メール TLS 1.2 自前実装 `kernel/tls.c` (RSA 鍵交換 + AES-128-GCM、証明書は SHA-256 フィンガープリントのピン留めで検証、合わなければ接続しない) と IMAP 受信、パスワードはディスク鍵で暗号化保存。(3) USB: UHCI で HID キーボード/マウス入力を通常入力へ注入し、BOT マスストレージをブロック層の2台目として接続。(4) 耐量子: `pq` コマンドで ML-KEM 鍵共有の実手順 (相手の公開鍵でカプセル化→指紋つきセッション鍵、seal/open で鍵共有+暗号化)、カーネル側 ML-DSA 署名、ML-KEM-512 公開ベクトル照合。(5) 互換レイヤー動的リンク (PT_DYNAMIC、DT_NEEDED の幅優先ロード、再配置、初期化関数)。(6) 監査ログ 80 件・AES-256-CTR 暗号化保存・ログイン前は平文ジャーナル preaudit.log に残しログイン後に統合。(7) 日本語フォント (Noto Sans CJK から JIS X 0208 をラスタライズ、6876 字、U+FFFD と全角スペース含む)。(8) 指紋・顔認証のソフトウェア側 `kernel/bio.c` (センサー捕捉は差し替え可能な境界、模擬入力で登録・照合・3 回失敗でロックアウトを検証。実センサーは実機確認待ち)。(9) UI: 重ね表示ウィンドウ (Notes/Calculator/Clock、移動・前面切替・閉じる)、クリップボード (テキスト)、右クリックメニュー、通知トースト、Dock 設定 (位置・サイズ・自動で隠す)、アプリランチャー、言語選択 (設定と初回導入時、ヘルプ切替)、JIS 配列。(10) ドライバ後付け: ELF64 再配置可能パッケージ (`-fpie -fno-plt`、GOT 経由で API 解決)、ML-DSA 署名必須、無効化リスト。アンロードは未対応。(11) 32bit 版: VGA テキスト画面 + PS/2 キーボード (ディスク・ネットワーク・図形デスクトップは未搭載)。(12) ARM 版: virtio-input キーボード + 画面コンソールで対話操作。未完了: xHCI、EHCI、Wi-Fi/Bluetooth ドライバ、内蔵アプリの隔離 (次節)、Prism Store 本体、標準アプリ不足分、トランプ全種、ブラウザ汎用化 | ⚠️ 大部分完了 (上記の未完了を除く。各項目は対応する QEMU テストで検証済み) |
| 残りの制限フェーズ追補4 (2026-11-22) | (25) メール MIME 土台 (shell/mime.c: base64/QP、encoded word、multipart、添付抽出) + フォルダ分け (mail-folders.txt) + 添付送受信 + 返信/転送/検索フィルタ。Date は実 TZ で生成。(26) 時計設定: CMOS RTC 書き込み (`date set`)、TZ 設定、NTP 変換 (rtc_from_unix/rtc_from_ntp)。NTP のライブ同期はこのサンドボックスの UDP 送信制限で未検証 (変換は単体検証済み、タイムアウトは graceful)。(27) diskfs v4: ファイルのタイムスタンプ (メタ領域) + 通常ファイルのチェックサム (FNV-1a、読取時検証)。v2/v3 互換維持。(28) アプリ内蔵アイコン: 20 アプリ + 汎用の 16x16 ピクセルアイコンを Dock/ランチャー/タスクバーに描画。(29) トランプ全種: ポーカー/バカラ/戦争/ババ抜き/7並べ/神経衰弱 (純ロジック kernel/cards.c、ホスト 115 件)。(30) 複数ユーザー + ゲスト + TOTP 2 要素 (RFC 4226/6238、SHA-1/HMAC 実装)。主鍵は各ユーザーの鍵でラップして users.txt に保持。ゲストは保存なしセッション。(31) サブディレクトリ + ゴミ箱: VFS/diskfs の名前規則で '/' を許可、mkdir/cd/trash 一式と Files の対応。(32) Office 上限: Sheet 26列x50行 (表示はビューポート追尾)、Slides 24 枚。(33) ICMP ping + ファイアウォール (fw.txt、送受信 IP 遮断)。(34) Prism Store: 署名必須パッケージ (ML-DSA-44、shell/storepkg.c) のカタログ取得・導入・削除、信頼ルートの配布 (store コマンド + Store アプリ)。(35) 標準アプリ追加: Contacts / Editor / Paint (BMP 保存)。アプリ総数 20、許可リスト SEC_APP_COUNT 20、Dock はボタン幅を自動縮小して全アプリを画面内に収める。(36) GUI の言語反映: L() でアプリ名・ランチャー・検索・右クリックメニュー・通知・設定ラベルを日英切替 (全リテラルの網羅は未完)。(37) 文字コード変換 (shell/charset.c: SJIS/EUC-JP/ISO-2022-JP→UTF-8、表は Python codecs 生成) を MIME と Files に組込み。(38) PDF (shell/pdf.c): FlateDecode 対応テキスト抽出 + 最小 PDF 生成 (Latin のみ)。(39) Undo/Redo と検索・置換 (Notes/Editor、Ctrl+Z/Y、Ctrl+F/H)。(40) ハイコントラスト設定 (アクセシビリティ)。(41) TCP サーバソケット (net_tcp_listen) と `serve` HTTP サーバ、`proxy` 設定とブラウザのプロキシ経由取得 (平文 HTTP のみ)。(42) fb 二重バッファ (PMM シャドウ + fb_flush)。(43) ARM ビルド修復: tls.c を ARM ソースへ追加、virtio_net_* 別名を arm/virtio_net.c に用意 | ✅ 検証済み (desktop-test、mail-test、store-test、serve-test、browser-test、login-test、clock-test、ping-test、compat-test、boot-test、arm-*、ホスト 37 スイート) |
| 残りの制限フェーズ追補3 (2026-11-22) | ブラウザ本格化・電源管理・性能実測。(1) ブラウザ: HTML 描画（リンクの Tab/クリック遷移、リダイレクト、chunked、CSS サブセット＝style 要素/インライン/font、24bit BMP 画像、HTTPS はメール用 TLS 流用＋証明書ピン留め TOFU を browser_pins.txt に保存）。`make browser-test` PASSED（CSS 色・画像をピクセル確認、証明書差し替えで拒否も確認）。JS は未実装、PNG/JPEG はデコーダなし（alt 表示）。(2) 電源管理: ACPI FADT/DSDT 解析（kernel/acpi.c）で S3 スリープ＋FACS waking vector 経由の本物の復帰（復帰スタブ kernel/acpi_wake.S を物理 0x9000 に配置、カーネル GDT/IDT と保存した CR3/スタックで acpi_s3_resume へ復帰）、S5 電源断。`power`/`sleep`/`poweroff` コマンド。`make power-test` PASSED（QEMU が suspended になることと復帰後のシェル応答を確認）。バッテリーは QEMU 標準マシンに機器が無く「なし」と正直に表示（実機の残量は AML 解釈が必要で未実装）。(3) 性能実測: docs/PERF_MEASUREMENT_2026-11-22.md（起動 5.6 秒、アプリ表示 0.4 秒弱、アイドル約 2MB） |
| 残りの制限フェーズ追補2 (2026-11-22) | (20) virtio-tablet (x86): モダン virtio PCI トランスポート (キャパビリティ走査、スプリット virtqueue) で絶対座標ポインタを PS/2 と併用可能に。(21) VBE 実行時解像度変更: シェル `res <w> <h>` で Bochs VBE モードを再設定しフレームバッファを張り替える (単一モニタのみ)。(22) USB ハブ (UHCI): ハブ記述子とポート電源/リセット/速度判定を実装し、ハブ下の HID/ストレージを列挙。中断 QH スロット割当の実バグも修正。(23) 32bit 版 ATA ディスク: LBA28 PIO 読み書き (`disk test` で検証)。32bit の永続 FS・ネットワーク・図形デスクトップは未搭載のまま。(24) 日本語入力 (IME) `kernel/ime.c`: ローマ字→かな変換 (促音・撥音・拗音) と自前の基本語彙辞書 (約570語、外部データ流用なし) による候補変換。Notes で F9 切替、変換中文字列と候補番号を画面下部に表示。文節分割・予測変換は行わない | ✅ 検証済み (tablet-test、res-test、hub-test、boot32-test、test_ime 58 件 + desktop-test の IME 手順) |
| 残りの制限フェーズ追補 (2026-11-22) | (13) シェル拡張: 履歴リスト、環境変数と $ 展開、パイプ (grep/tee/run/cat)、スクリプト実行、ゲストへの stdin 投入。(14) virtio-net (x86): レガシー virtio PCI、netstack に NIC 抽象 (e1000 優先)。(15) NVMe: 管理/IO キュー、PRP 転送、ブロック層へ統合 (システム候補 + 機器 2)、`disk test` で検証。(16) 起動アニメーション: ロゴのフェードイン + プリズム色ドット + 進捗バー (約 2.2 秒、整数演算のみ、フレーム差分をテストで実測)。(17) SMP: MADT 解析、LAPIC、INIT/SIPI + 実モードトランポリンで AP 起動 (4 CPU 検証済み)。AP は IPI (ベクタ 0x40) に応答する。スケジューラはまだ無く、仕事の分担はしない。(18) HD Audio (Intel HDA): Immediate Command でコーデック列挙と出力経路設定、BDL/DMA 再生、`hda beep` でトーン再生 (WAV キャプチャで実証)。(19) SD/SDHCI: PIO 転送でカード初期化 (SDSC の CSD v1 / SDHC の CSD v2 両対応)、ブロック層の機器 3 として接続 | ✅ 検証済み (nvme-test、net-test、boot-anim-test、smp-test、hda-test、sdhci-test) |
| v0.3 | デスクトップの土台 (画面描画、壁紙、タスクバー、Dock、画面アプリ) | ✅ 完了 |
| v0.4 | テーマエンジン、ファイルシステム | 計画 |
| v1.0 | 実機ブート、アプリ実行、設定アプリ | 計画 |

## ビルドとテスト

- `make` — カーネル + ISO (`build/prismos.iso`)
- `make test` — ホストテスト (シェル、メモリ、デスクトップ、
  永続化、ネットワーク、マウス、暗号化)
- `make boot-test` — QEMU で ISO を起動し、バナーとシェル出力を確認
- `make boot-test-kbd` — キーボード入力を QEMU で確認
- `make desktop-test` — デスクトップの画面を QEMU で確認
- `make install-test` / `make login-test` — インストールと
  ログインを QEMU で確認
- `make crypto-test` — ファイル暗号化を QEMU で確認
  (ディスクの生データに平文が無いこと、再起動後のログインで
  読めること)
- `make security-test` — セキュリティを QEMU で確認
  (v2 アカウントでのログイン、監査ログ、ロックアウト、
  外部メディア制限、v1 から v2 への移行、リモートロック)
- `make disk-boot-test` — ディスク単体での起動を QEMU で確認
- `make net-test` — ネットワークを QEMU で確認
  (ホスト側に HTTP サーバが要る)
- `make check-asm` — カーネルに SSE/MMX/x87 命令が無いことを確認
- GitHub Actions (`.github/workflows/build.yml`) が `make test` /
  `make` / `make boot-test` を `ubuntu-latest` で回す。ISO は
  Artifact として保存される。

## 内蔵デスクトップアプリの隔離について（2026-11-22 検討）

内蔵の画面アプリ（ターミナル、ファイル、メモ、ブラウザ等）は、カーネルと同じアドレス空間で動く関数群として実装されている（`kernel/desktop.c` がモード切替で各アプリの描画・キー処理を呼ぶ）。そのため、あるアプリのメモリ破壊がそのままカーネルや他アプリに及ぶ構造であり、互換ゲストのような隔離は効いていない。

隔離を実現するには、アプリごとにページテーブル（アドレス空間）を持ち、描画と入力を IPC で仲介するプロセスモデルへの移行が必要になる。現在のデスクトップは共有メモリ上の状態（設定、クリップボード、通知、ファイル一覧）を直接読み書きしており、この移行はデスクトップ全体の再設計になるため、本フェーズでは行わない。互換レイヤーのゲストについては、専用ページテーブル + リング 3 + システムコール制限で隔離が実現済みであり、外部アプリの実行はそちらが担う。内蔵アプリの隔離はチェックリスト項目13の残件として明示する。
