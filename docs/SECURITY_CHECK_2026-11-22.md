# PrismOS 並行網羅チェック結果（2026-11-22）

開発中ツリーのコピー（/tmp/prism-check/）で実施。開発ツリー自体は無変更。コピーは5種ビルド作業中のスナップショット（kernel32/ が存在）。`make test` 総数 2497 は Wave 6 のベースラインと一致。

## 1. ISO起動テスト結果（コピー側、順次実行）

| テスト | 結果 |
|---|---|
| `make test` | **2497 checks, 0 failures**（exit 0） |
| `make boot-test` | **BOOT TEST PASSED**（13項目 ok） |
| `make desktop-test` | **DESKTOP TEST PASSED** |
| `make checklist` | **合格 5 / 不合格 4 / 未検証 4**（全13項目） |

checklist 内訳：合格＝1, 2, 7, 9, 10。不合格＝4（ショートカットGUI編集なし）、6（サンドボックス未実装）、12、13（残り制限17件）。未検証＝3（実機系）、5（初心者実測）、8（8時間）、11（Actions）。

**項目12の不合格は手順上のもので、Wave 6 からの後退ではない。** コピー側で ISO を再ビルドしたため、既存の署名ファイル（旧ISO向け）が一致しなくなった。再署名（iso-sign）は署名鍵を使うため本チェックでは実行していない。SHA-256 チェックサム自体は一致=True。開発側で `make checklist` を通常通り回せば（iso-sign 込み）Wave 6 と同じく合格になるはず。

**環境の注意点**：この環境のシステム python3 には `cryptography` モジュールが無く、`make checklist` は iso-sign で `ModuleNotFoundError` になり止まる（VM交換で消えた可能性）。開発環境側で python3-cryptography の復旧が必要。

## 2. 0Day／脆弱性チェック結果（公開情報のみ）

**該当なし**：Limine 8.3.2 への公開アドバイザリは確認した情報源の範囲でなし。CISA KEV の2026年11月追加分24件に QEMU／Limine／gcc／Python／Actions ランナーの項目はなし。CPython 本体の重大アドバイザリは確認できず。liboqs の新規アドバイザリ（CVE-2026-44519 FrodoKEMタイミング、2026-11-06公開／CVE-2026-44518・46344 XMSS境界外読み）は、PrismOS は liboqs を使わず自前実装のため**対象外**。

**QEMU（ローカル8.2.2／CI 10.2.1）— 確認できた公開アドバイザリ**。いずれも QEMU ホストプロセス側の問題で、PrismOS 本体の欠陥ではない：
- CVE-2026-93834（9pfs use-after-free、CVSS 8.8、2026-09-21公開、VMエスケープ）：PrismOS は 9pfs（-virtfs/-fsdev）を使っていないため**到達不可**。
- virtio-gpu 整数オーバーフロー群（ZDI-CAN-27578、対象 8.1.0〜10.2.3、修正は 11.0.0 のみ、公開エクスプロイト解説あり）：現行テストは virtio-gpu を使わないため到達不可。**ただし virtio-gpu は今後のハードウェア計画にあるため、採用時は QEMU 11 以降が前提**。
- CVE-2026-48914（virtio-blk ヒープオーバーフロー、CVSS 6.7、2026-06-12公開）：virtio-blk は driver-test で使用中。到達可能な構成。
- CVE-2025-12464（e1000 スタックオーバーフロー、ゲスト→QEMUクラッシュ）：e1000 は標準NICとして使用中。
- Ubuntu 系バンドル（2026-04-08）：UHCI を不正状態にしてゲストから QEMU を落とせる問題。UHCI は driver-test で使用中。desktop／checklist テストは `-display vnc` で QEMU を起動（ホスト側テスト基盤の露出）。
- CVE-2026-0665（Xen PHYSDEVOP 境界外、CVSS 6.5）：Xen エミュレーション不使用のため対象外。

**binutils**：CVE-2026-6846（XCOFF リンク時ヒープオーバーフロー、High）、CVE-2026-15003（ld 境界外読み 5.6）、CVE-2026-6844／6845（readelf DoS）、CVE-2026-90828（2.47 ldelf NULL参照 6.6）。いずれも細工されたオブジェクトファイルの処理が前提で、PrismOS は自前ソースのみをビルドするため実務リスクは低い。gcc 本体の CVE は確認できず。

## 3. バグ精査結果（読み取り専用レビュー＋ASan/UBSan 動的検査）

動的検査は `make test` を `-fsanitize=address,undefined -fno-sanitize-recover=all` で再ビルドして実施。B1〜B4 は製品コードの新規発見で、2026-11-21 監査には載っていない。

- **B1（中）shell/zip.c:276-277 `zip_read_entry`**：境界チェックの整数オーバーフロー。`uint32_t off` に対し `off + 30 > len` を32bitで計算するため、細工した ZIP 中央ディレクトリで local_offset ≥ 0xFFFFFFE2 にすると合計が巻き戻ってチェックを通過し、`rd32(data + off)` がバッファの約4GB先を読む。local_offset は読み込む .docx/.pptx から攻撃者が制御可能。parse 側の `cd_offset + cd_size` も同じ形だが後段の size_t チェックで実質塞がっている。
- **B2（中・将来必須）shell/mlkem.c:495-496 `kem_decaps`**：FO 暗黙拒否パスのタイミングチャネル。再暗号化との比較に早期終了する `memcmp` を使い、結果で `k2`/`kbar` のどちらを読むかが変わる。公開済みの Kyber 系タイミング問題と同型。現状 ML-KEM は部品のみで鍵交換に未使用のため実害は限定的だが、プロトコル接続前に修正必須。
- **B3（中）shell/docx.c:604、同:992、shell/pptx.c:409**：タグ長クランプ漏れによる境界外読み。タグ文字列は固定バッファ（docx 側 tag[128]／tag[64]、pptx 側 tag[32]）にクランプしてコピーするのに、self_close 判定はクランプ前の `tag[tag_len - 1]` を読む。バッファより長いタグを含む .docx/.pptx を読み込むとスタック外を読む。**UBSan が通常テスト中に docx.c:992 で「char[64] に対する index 80」を実際に検出**。実物の Office ファイルは属性文字列が長く日常的に到達可能で、最悪 tag_len はファイルサイズ（16384）近くになりカーネルでクラッシュし得る。
- **B4（低）shell/update.c:42-59 `update_hex_to_bytes`**：短い16進文字列の終端超え読み。`hi` の成否を確認する前に `hex[i*2+1]` を無条件に評価するため、終端が偶数位置にある文字列で NUL の1バイト先を読む。**ASan が既存テストケース `update_hex_to_bytes("ba78", …)` で global-buffer-overflow（update.c:48）を検出**。
- **B5（テスト側）tests/test_video.c:218-227**：`ops` 配列は12バイト宣言なのに14バイト書き込む（テスト自身のスタック破壊。UBSan 検出）。
- **B6（テスト側・軽微）tests/test_fat16.c:106**：build_image の 5MB calloc が解放されずリーク（LeakSanitizer 検出、製品 fat16.c は無関係）。

健全と確認した範囲：ZIP中央ディレクトリ解析の境界、互換ELFローダの検証（filesz≤memsz、offset+filesz≤len、memsz≤1MB）、inflate の出力境界、mailproto のコマンド組み立て境界。上記以外の23テストスイートは ASan/UBSan でもクリーン。

## 4. 修正方針（開発側へ引き継ぎ済み 2026-11-22）

- **B1**：`zip_read_entry` のチェックを `(size_t)off + 30 > len` に変更。parse 側 `cd_offset + cd_size` も size_t 化。回帰テストを test_zip に追加。
- **B2**：XOR 累積の定数時間比較＋マスクによる分岐なし鍵選択に置き換え。ML-KEM を鍵交換へ接続するフェーズの前提条件。
- **B3**：3箇所とも self_close を元文字列から判定（`tag_len > 0 && s[end - 1] == '/'`）。長い属性つきタグの回帰テストを追加。
- **B4**：hi が負なら lo を評価せず即 -1 を返す。
- **B5**：`ops` の宣言を必要サイズ（14バイト）に修正。**B6**：テスト終了時に free を追加。
- **基盤**：開発環境の python3 に cryptography を復旧。virtio-gpu 採用時は QEMU 11 以降を前提条件にする。
