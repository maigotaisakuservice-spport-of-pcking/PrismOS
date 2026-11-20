# PrismOS — どこでもカスタムできる OS

> **UI は Windows 11 風、機能は Mac 風、シェルは Linux と Windows 両対応。**
> テーマ・壁紙・タスクバーの位置は `config/theme.toml` で変えられる。

PrismOS は、見た目も動きも自分好みに変えられる OS を目指すプロジェクトです。
最初の製品版は **27H1（2027年度上半期）** に出す予定です。
開発版は v0.1 → v0.9 と進み、v1.0 到達をもって製品版とします。

v0.1 は「QEMU で起動して、シリアル経由のシェルが動く」最小カーネルです。
GUI は後のフェーズで、設計は `docs/DESIGN.md` にあります。
製品版の仕様はすべて `docs/PRODUCT_v1.0_SPEC.md` にまとめてあります。

## 製品版で目指すもの

- **デスクトップ:** Windows 11 風のスタート・タスクバー（既定は左寄せ）と、Mac 風の Dock・Spotlight 風検索・Mission Control 風一覧。Dock は最初から表示する。ウィジェット（天気・ニュース・カレンダー）も出す
- **標準アプリ:** 自作ブラウザ「Prism Browser」（Chromium ではなく一から作るエンジン、既定の検索は Google）、独自オフィス「Prism Office」（Office 形式とも互換）、メール・カレンダー・連絡先・メモ・時計、写真・動画・音楽プレイヤー、3D シューティングゲームとトランプ
- **ストア:** 「Prism Store」。アプリは無料だけ。配信・出品するのはこちら（PrismOS 側）のみ
- **アップデート:** セキュリティ更新は自動、機能更新は手動で分ける。ベータチャンネルも選べて、安定版に戻れる。失敗したら自動で前の版に戻る。配信元は GitHub Releases のみ。バージョン番号は Windows 風の `27H1` 形式
- **セキュリティ:** 市役所レベルの Enterprise レベルを標準で適用する。変更は管理者権限があるアカウントだけができる
- **互換性:** Wine のように、Windows / Linux / macOS のアプリをそのまま動かす互換層を目指す。Mac のアプリ（.app / .dmg）も対象
- **その他:** アカウントはローカルのみ。言語は日本語と英語から選べる。ARM 版も x86 版と同じタイミング・同じ機能で出す。デュアルブート版の ISO は別で作る

## v0.1 でできること

- Limine 8.3.2 でブートし、QEMU で起動する
- 起動バナーをシリアル (COM1) に出す
- シリアルシェルで Linux / Windows 両方のコマンドを使う

| やりたいこと | Linux 風 | Windows 風 |
|---|---|---|
| ファイル一覧 | `ls` | `dir` |
| ファイル表示 | `cat README.txt` | `type README.txt` |
| 画面クリア | `clear` | `cls` |
| バージョン | `ver` | `version` |

そのほか `pwd` `cd` `echo` `help` `about` `theme` `reboot` が使えます。

## ビルド方法

必要なもの: `gcc` `make` `curl` `xorriso` `mtools`、QEMU を使う場合 `qemu-system-x86`

```sh
# Ubuntu / Debian の例
sudo apt-get install -y gcc make curl xorriso mtools qemu-system-x86

# カーネル + ISO を作る (Limine 8.3.2 は自動でダウンロードされる)
make

# シェルのユニットテスト (ホスト側、QEMU 不要)
make test

# QEMU で起動する
make run

# QEMU で起動してバナーとシェルを自動で確認する
make boot-test
```

成果物: `build/kernel.elf`、`build/prismos.iso`

## 構成

```
PrismOS/
├── kernel/          C の最小カーネル (main, シリアル, シェルラッパー, Limine)
│   └── limine.h     Limine 8.3.2 のヘッダ (ベンダリング済み)
├── shell/           Linux/Windows 両対応シェルのコア (カーネルとテストで共有)
├── config/
│   └── theme.toml   テーマ・壁紙・タスクバー・Dock の設定例
├── docs/
│   ├── DESIGN.md              アーキテクチャとフェーズ計画
│   └── PRODUCT_v1.0_SPEC.md   製品版 v1.0 の仕様書
├── tests/           シェルパーサーのホストテスト
└── .github/workflows/build.yml   ビルド + QEMU 起動テスト (GitHub Actions)
```

## カスタマイズ

`config/theme.toml` を編集すると、将来の GUI が読む設定を変えられます。
v0.1 ではシェルの `theme` コマンドがこのファイルの内容を表示します。

```toml
[theme]
accent = "#4CC2FF"          # アクセントカラー
wallpaper = "prism-dusk.png"
taskbar_position = "bottom" # top / bottom / left / right
```

詳しくは `config/theme.toml` と `docs/DESIGN.md` を見てください。

## ロードマップ

- **v0.1 (いまここ)**: 最小カーネル + Limine ブート + シリアルシェル
- **v0.2**: キーボード入力、メモリ管理、ファイルシステムの土台
- **v0.3**: フレームバッファ描画、Windows 11 風デスクトップの土台、Dock
- **v0.4**: ファイルマネージャー、設定アプリ、ターミナル
- **v0.5**: ネットワークとデバイス
- **v0.6**: テーマエンジン、プリセット、プラグイン
- **v0.7**: 暗号化、サンドボックス、権限、署名付き更新
- **v0.8**: 標準アプリ完成（Prism Browser、Prism Office、写真、音楽）
- **v0.9**: インストーラー、バックアップ、復旧、安定性の仕上げ
- **27H1 (製品版 v1.0)**: この仕様書の全項目を満たし、Windows 11 レベルで日常利用できること

開発版 v0.2〜v0.9 は GitHub Releases には出しません。製品版 27H1 まで待ちます。

## Q&A（よくある質問）

サポートはこの README の Q&A のみです。チャットや電話でのサポートはありません。

**Q. PrismOS はどんな OS ですか？**
A. Windows 11 風の UI、Mac 風の機能、Linux と Windows 両対応のシェルを持ち、どこでもカスタムできる OS です。最初の製品版は 27H1 です。

**Q. 今の v0.1 で何ができますか？**
A. QEMU で起動して、シリアルシェルで `ls` / `dir` などのコマンドが使えます。GUI はまだありません。

**Q. どうやってビルドしますか？**
A. 上の「ビルド方法」を見てください。`make test` でシェルのテスト、`make boot-test` で QEMU の起動テストができます。

**Q. Windows や Mac のアプリは動きますか？**
A. 製品版では Wine のような互換層で動かすのが目標です。v0.1 ではまだ動きません。

**Q. インストールはどうしますか？**
A. 製品版では USB からインストールできます。通常版とデュアルブート版の ISO を 2 種類作ります。

**Q. 更新はどうやって届きますか？**
A. GitHub Releases から届きます。セキュリティ更新は自動、機能更新は手動で、失敗したら自動で前の版に戻ります。

## ライセンス

MIT License — `LICENSE` を参照。
