/* PrismOS — ディスクへの永続化とインストーラ
 *
 * ATA ディスク上の diskfs 形式と、RAM 上の VFS の橋渡し。
 *   * persist_init(): 起動時に ATA を探し、結果をシリアルに
 *     出す。ディスクが無くても起動は続く。
 *   * persist_load(): インストール済みなら、ディスクの全
 *     ファイルを VFS に載せる (組み込みファイルは置き換える)。
 *     インストールされていなければ何もしない。
 *   * persist_maybe_sync(): VFS が変わっていたら、変わった
 *     内容をディスクに全部書き戻す。未インストールなら何もしない。
 *   * persist_install(): ディスクを初期化し (MBR + 形式)、
 *     今の VFS の全ファイルを書き込む。本物のインストーラ。
 *   * 暗号化: ログインで確かめた PIN から鍵を作り
 *     (反復ハッシュ、フェーズ14 から。以前は SHA-256 1 回)、
 *     ".account" と "settings.txt" 以外のファイルの
 *     中身を AES-256-CTR で暗号化してディスクに書く。
 *     ".account" と "settings.txt" は平文 (ログイン前・
 *     起動時に読むため。settings.txt に秘密情報は無い)。
 *     IV はファイルごとに新しく作り、ファイル表に置く。
 *     鍵が無い間 (ログイン前) は、暗号化されたファイルは
 *     暗号文のまま VFS に載せ、ログイン後に復号する。
 *   * 外部メディア制限: settings.txt の usb_media=block
 *     のとき、起動時にディスクの利用者ファイルを読まず、
 *     書き戻しも settings.txt と .account だけにする。
 *     インストールも拒否する。
 */
#ifndef PRISM_PERSIST_H
#define PRISM_PERSIST_H

#include <stdint.h>

#include "diskfs.h"

/* ログインで確かめた PIN から作った鍵 (32 バイト) を渡し、
 * ディスク暗号化を有効にする。 */
void persist_set_key(const uint8_t *key32);
int persist_key_ready(void);
int persist_get_key(uint8_t *out32);

/* 鍵を捨てて暗号化を無効にする (メモリからも消す)。 */
void persist_clear_key(void);

/* 暗号化が有効かどうか。 */
int persist_key_active(void);

/* 起動時に暗号文のまま読み込んだファイルを、鍵で復号する。
 * 復号したファイルの数を返す。鍵が無ければ 0。 */
int persist_decrypt_loaded(void);

/* 指定したファイルが「暗号文のまま読み込まれ、まだ復号されて
 * いない」状態かどうか。監査ログのように、復号されるまで
 * 中身へ追記できないファイルの判定に使う。 */
int persist_name_pending_decrypt(const char *name);

/* Wrap a short secret (e.g. a mail password) with the
 * disk key: out receives "hex(iv):hex(ciphertext)".
 * Needs the key active; returns 0 on success. */
int persist_wrap_secret(const char *plain, char *out, size_t cap);

/* Inverse of persist_wrap_secret. Returns 0 on success,
 * -1 when the key is inactive or the format is bad. */
int persist_unwrap_secret(const char *enc, char *out, size_t cap);

/* ---- 全ディスク暗号化 (FDE、docs/FDE_DESIGN.md) ---- */

/* ディスクに FDE ヘッダがあるかどうか (persist_init で判定)。 */
int persist_fde_present(void);

/* ヘッダがあり、マスター鍵がメモリにあって読み書きできる。 */
int persist_fde_unlocked(void);

/* 移行 (有効化/解除) が途中で止まっているかどうか。
 * 0 = なし、1 = 暗号化の途中、2 = 復号の途中。 */
int persist_fde_migration_pending(void);

/* パスフレーズ (スロット0 か、あればスロット1 のリカバリ
 * キー) が正しいかどうかだけを確かめる。1 = 正しい。 */
int persist_fde_verify(const char *passphrase);

/* パスフレーズでロックを解除する。戻り値: 1 = 解除して
 * diskfs を調べた、2 = 解除したが移行の再開が先 (続けて
 * persist_fde_resume_migration() を呼ぶこと)、0 = FDE
 * でない、-1 = パスフレーズが違う。 */
int persist_fde_try_unlock(const char *passphrase);

/* 止まっていた移行を続きから行い、終わったら diskfs を
 * 調べる。復号 (解除) の移行ならヘッダも消す。0 = 成功。 */
int persist_fde_resume_migration(void);

/* FDE を有効化する: マスター鍵を作ってパスフレーズで包み、
 * 全セクタをその場で暗号化する (ジャーナル付き)。
 * recovery_out (25 バイト以上) に管理者リカバリキーを返す。
 * ディスクが無い・未インストール・既に FDE なら -1。 */
int persist_fde_enable(const char *passphrase, char *recovery_out,
                       size_t recovery_cap);

/* FDE を解除する: 全セクタを復号してヘッダを消す。
 * ロック解除済みのときだけ。0 = 成功。 */
int persist_fde_disable(void);

/* ATA を探す。起動時に 1 回呼ぶ。 */
void persist_init(void);

/* ディスクがあるかどうか / インストール済みかどうか。 */
int persist_disk_present(void);
int persist_installed(void);

/* インストール済みならディスクから VFS に載せる。
 * 載せたファイル数を返す。未インストールなら 0。 */
int persist_load(void);

/* VFS が前回の書き戻しから変わっていたら全部書き戻す。
 * 書き戻したら 1、しなければ 0、失敗したら -1。
 * 外部メディア制限のときは settings.txt と .account だけを
 * 書き戻す (利用者ファイルは触らない)。 */
int persist_maybe_sync(void);
/* ゲストセッション用: 同期を止める (変更を保存しない)。 */
void persist_block_sync(int on);

/* 変わっているかどうかに関わらず全部書き戻す。
 * 書き戻したら 1、ディスクが無い・未インストールなら 0、
 * 失敗したら -1。PIN 形式の移行など、鍵が変わった直後に
 * 全ファイルを新しい鍵で書き直すときに使う。 */
int persist_force_sync(void);

/* 外部メディア制限が有効かどうか (1 = 有効)。
 * persist_load() が起動時にディスクの settings.txt を
 * 読んで決める。 */
int persist_media_blocked(void);

/* ディスクを初期化して今の VFS を書き込む。
 * 成功したら 0。ディスクが無ければ -1、失敗したら -2。 */
int persist_install(void);

/* 構成を指定してディスクにインストールする。minimal = 1 は
 * 最小構成 (ブラウザ・ファイル管理・設定だけ)。データ用
 * パーティションがある盤ではそこだけを書き、MBR も起動用
 * パーティションも触らない。成功したら 0。ディスクが無ければ
 * -1、失敗したら -2。 */
int persist_install_filtered(int minimal);

/* インストールの検証: 構成に入る VFS のファイルが全部ディスク
 * にあるか確かめる。全部あればそのファイル数を、1 つでも
 * 無ければ -1、ディスクが無い・未インストールなら -2 を返す。 */
int persist_verify_install(int minimal);

/* ---- インストーラー用のディスク情報 ---- */

/* ディスクの機種名 (ATA のモデル文字列)。無ければ ""。 */
const char *persist_disk_model(void);

/* ディスクの総セクタ数。無ければ 0。 */
uint32_t persist_disk_sectors(void);

/* データ用パーティションがある盤かどうか (起動できる盤)。 */
int persist_data_partition_present(void);

/* MBR (セクタ 0) を 512 バイト読む。成功したら 0。 */
int persist_read_mbr(uint8_t *buf512);

/* デュアルブートのディスクのデータパーティションだけを
 * 初期化して今の VFS を書き込む。MBR も起動用パーティション
 * も触らない。成功したら 0。ディスクが無ければ -1、
 * データパーティションが無ければ (デュアルブートの盤で
 * なければ) -2、失敗したら -3。 */
int persist_install_dual(void);

/* ディスクを初期化する (回復モードの「初期化」用)。
 * パーティションがある盤ではデータパーティションだけを、
 * 無い盤ではディスク全体を初期化する。成功したら 0。
 * ディスクが無ければ -1、失敗したら -2。 */
int persist_initialize_disk(void);

/* ---- 回復モードの要求フラグ ---- */

/* 次の起動で回復モードに入るよう、ディスクに印を付ける。
 * 成功したら 0。ディスクが無い・未インストールなら -1。 */
int persist_request_recovery(void);

/* persist_init() で読んだ印。1 = 今回の起動は回復モード。 */
int persist_recovery_requested(void);

/* ---- 復元ポイント (スナップショット) ---- */

/* 今の時刻で復元ポイントを作る。名前は呼び出し側が付ける。
 * 成功したら 0。ディスクが無い・未インストールなら -1、
 * それ以外は diskfs_snapshot_create() と同じ。 */
int persist_snapshot_create(const char *name);

/* 復元ポイントの一覧。成功したら 0、*out_count に件数。 */
int persist_snapshot_list(diskfs_snapshot_info *infos, int cap,
                          int *out_count);

/* 復元ポイントをディスクに書き戻す。成功したら 0。
 * 無ければ -1、壊れていれば -2。 */
int persist_snapshot_restore(const char *name);

/* 復元ポイントを消す。成功したら 0、無ければ -1。 */
int persist_snapshot_delete(const char *name);

/* 今の時刻を "YYYY-MM-DD HH:MM:SS" で buf (20 バイト以上) に
 * 入れる。時計が読めなければ "0000-00-00 00:00:00"。 */
void persist_timestamp_now(char *buf);

#endif /* PRISM_PERSIST_H */
