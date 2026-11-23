/* PrismOS — 生体認証 (指紋・顔) のソフトウェア側。
 *
 * テンプレートの登録・照合・失敗時ロックアウトと、
 * ロック解除フローへの接続を提供する。センサーからの
 * 取り込みは bio_sensor_capture() の 1 関数に閉じてあり、
 * 実センサーのドライバはこの関数を差し替えるだけでよい
 * (現在の実装はテスト用の模擬センサー: bio_mock_present()
 * で「かざされた試料」を切り替える決定的な生成器)。
 *
 * テンプレートは bio.db に保存する。通常の利用者ファイル
 * なので、PIN 設定時はディスク上で暗号化される (平文の
 * 例外リストには入れない)。
 *
 * 起動時ログインで生体だけを使わない理由: ディスク鍵は
 * PIN から作られ、bio.db 自体もその鍵で暗号化されるため、
 * PIN の前にテンプレートを読むことができない。生体解除は
 * 鍵が有効な間の再ロック解除 (lock コマンドとロック画面)
 * に使う。これはセキュアエンクレーブを持たない実装での
 * 正直な制約である。
 */
#ifndef PRISM_BIO_H
#define PRISM_BIO_H

#include <stdint.h>

#define BIO_FEAT_LEN 32
#define BIO_TYPE_FINGER 1
#define BIO_TYPE_FACE 2
#define BIO_LOCK_THRESHOLD 3

/* センサー: 試料があれば 0 を返して feat を埋める。
 * 何もかざされていなければ -1。 */
int bio_sensor_capture(uint8_t feat[BIO_FEAT_LEN]);

/* 模擬センサーの制御 (テスト用): id > 0 の試料をかざす。
 * id = 0 で「何もかざされていない」にする。 */
void bio_mock_present(int id);
int bio_mock_current(void);

/* 登録: 現在の試料を取り込んで type のテンプレートにする。
 * 0 = 成功、-1 = 試料なし、-2 = 保存に失敗。 */
int bio_enroll(int type);

/* 照合: 0 = 一致、-1 = 不一致、-2 = ロック中、
 * -3 = 未登録、-4 = 試料なし。 */
int bio_verify(int type);

/* 登録の有無と状態。 */
int bio_enrolled(int type);
int bio_fail_count(void);
int bio_locked(void);

/* PIN で正しく入れたときに失敗回数とロックを戻す。 */
void bio_unlock_reset(void);

/* テンプレートを全部消す。0 = 成功。 */
int bio_clear(void);

#endif /* PRISM_BIO_H */
