/*
 * 後付けドライバのローダ (ドライバインストール機構)。
 *
 * パッケージは ELF64 の ET_REL オブジェクトで、-fpie -fno-plt
 * でビルドすること (外部参照がすべて GOT 経由になり、カーネル
 * ヒープとカーネル像が ±2GB の範囲になくても届く)。
 * 未定義シンボルは
 * カーネルのドライバ API 表からのみ解決し、RELA 再配置を
 * 適用してから prism_driver_init(api_version) を呼ぶ。
 * 読み込み前に ML-DSA-44 署名の検証が必須 (呼び出し側)。
 *
 * 正直な境界: ドライバはリング 0 で動く。署名が信頼の仕組みで、
 * 署名済みでも壊れたドライバはカーネルを落としうる。回復手段は
 * 無効化リスト (drivers.disabled) で、起動時・導入時に照合する。
 */
#ifndef PRISM_DRVLOAD_H
#define PRISM_DRVLOAD_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_DRV_API_VERSION 1
#define DRV_MAX_LOADED 8

/* ドライバ API (パッケージが参照できる唯一のシンボル群)。 */
void prism_drv_log(const char *msg);
void *prism_drv_alloc(size_t n);
void prism_drv_free(void *p);
int prism_drv_vfs_write(const char *name, const char *data, size_t len);
int prism_drv_vfs_read(const char *name, char *buf, size_t cap);
uint64_t prism_drv_tick(void);

/* ET_REL パッケージを読み込み、init を呼ぶ。
 * 戻り値: 0 成功、負数 = エラー
 *   -1 形式が違う (ELF でない / ET_REL でない / 64bit でない)
 *   -2 セクション・シンボル表が壊れている
 *   -3 未定義シンボルが API 表にない
 *   -4 再配置の種類が未対応
 *   -5 prism_driver_init がない、または init が失敗した
 *   -6 読み込み済みドライバの上限
 *   -7 メモリ不足
 */
int drv_load(const char *name, const uint8_t *image, size_t len);

/* 読み込み済みドライバの数と名前。 */
int drv_loaded_count(void);
const char *drv_loaded_name(int i);

#endif /* PRISM_DRVLOAD_H */
