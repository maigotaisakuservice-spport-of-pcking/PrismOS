/* PrismOS — Prism Store のカーネル側共通処理
 *
 * シェルの store コマンドとデスクトップの Store
 * アプリの両方が使う。パッケージ形式は
 * shell/storepkg.h、署名は ML-DSA-44 (信頼の根は
 * VFS の store.pub)。
 */
#ifndef PRISM_STORE_H
#define PRISM_STORE_H

#include <stddef.h>
#include <stdint.h>

/* パッケージ (メモリ上) を署名検証して導入する。
 * 成功したら 0、失敗理由を err に書く。導入の通知は
 * シリアルと監査ログへ出す。 */
int prism_store_install_mem(const uint8_t *data, size_t len,
                            char *err, size_t errcap);

/* 導入済みアプリを記録ごと削除する。成功したら 0。 */
int prism_store_uninstall(const char *name);

/* store.conf ("host port base") を読む。成功したら 0。 */
int prism_store_server(char *host, size_t hcap, int *port_out,
                       char *base, size_t bcap);

#endif /* PRISM_STORE_H */
