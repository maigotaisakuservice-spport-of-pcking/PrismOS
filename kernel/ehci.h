/* PrismOS — EHCI USB 2.0 ホストコントローラ
 *
 * PCI クラス 0C/03/20 (EHCI) を初期化し、ルートハブの
 * ポートに繋がった機器を列挙する。非同期スケジュール
 * (QH + qTD) だけで制御転送とバルク転送を回すポーリング
 * 実装。対応する実動作はマスストレージ (Bulk-Only
 * Transport + SCSI) で、ブロック層へ追加ディスクとして
 * 登録する。HID の割り込み転送 (周期スケジュール) は
 * 未実装 — HID は UHCI 側が担当する。
 *
 * 純粋な組み立て関数 (ehci_qtd_token / ehci_qh_epchar /
 * ehci_parse_config) はハードウェアに触らないので、
 * ホスト側のユニットテストでも同じソースを動かせる。
 */
#ifndef PRISM_EHCI_H
#define PRISM_EHCI_H

#include <stddef.h>
#include <stdint.h>

#define EHCI_MAX_DEVICES 4

/* 列挙した 1 台の情報。 */
typedef struct {
    uint8_t address;    /* 割り当てた USB アドレス */
    uint8_t port;       /* 繋がっているポート (1 始まり) */
    uint16_t vendor;    /* 売主 ID */
    uint16_t product;   /* 製品 ID */
    uint8_t dev_class;  /* 機器の種別 */
    uint8_t if_class;   /* 最初の接続面の種別 (0 = 不明) */
    uint8_t max_packet; /* 制御転送の最大パケット */
    int speed;          /* 0=全速、1=低速、2=高速 */
    /* ここから構成設定後に埋まる欄。 */
    uint8_t if_num;       /* ストレージ接続面の番号 */
    uint8_t bulk_in_ep;   /* ストレージ用バルク終端 */
    uint8_t bulk_out_ep;
    uint16_t bulk_max_packet;
    uint32_t storage_sectors; /* 512 バイト単位の容量 */
} ehci_device;

/* EHCI を探して初期化し、機器を列挙する。HHDM のずれを
 * 渡す。見つかれば 1、無ければ 0 を返す。 */
int ehci_init(uint64_t hhdm_offset);

/* コントローラが見つかったかどうか。 */
int ehci_present(void);

/* 列挙した機器の数。 */
int ehci_device_count(void);

/* i 番目の機器の情報。範囲外なら NULL。 */
const ehci_device *ehci_device_at(int i);

/* USB マスストレージがあるかどうか (列挙 + INQUIRY +
 * READ CAPACITY まで成功したもの)。 */
int ehci_storage_present(void);

/* ストレージの容量 (512 バイトセクタ数)。 */
uint32_t ehci_storage_sectors(void);

/* ストレージの 1 セクタ読み書き。成功 0。 */
int ehci_storage_read_sector(uint32_t lba, uint8_t *buf);
int ehci_storage_write_sector(uint32_t lba, const uint8_t *buf);

/* ---- 純粋関数 (ホストテスト用) ---- */

/* qTD トークンを作る。pid は 0=OUT / 1=IN / 2=SETUP、
 * len は転送バイト数、toggle はデータトグル、ioc は
 * 完了割り込みビット。状態ビットは「動作中」を立てて
 * 返す (ハードウェアが進捗に応じて消す)。 */
uint32_t ehci_qtd_token(int pid, uint32_t len, int toggle, int ioc);

/* QH の接続点特性 (第 2 ワード) を作る。addr は機器
 * アドレス、ep は終端番号、speed は 0=全速/1=低速/
 * 2=高速、max_packet は最大パケット長、head は
 * 回収リスト先頭ビット、control は全速/低速の制御
 * 終端なら 1。 */
uint32_t ehci_qh_epchar(uint8_t addr, uint8_t ep, int speed,
                        uint16_t max_packet, int head, int control);

/* 構成記述子 (全部) を歩いて、マスストレージ接続面の
 * 終端情報を info に入れる。見つかれば 0。UHCI 側と
 * 同じ歩き方だが、最大パケットは 16 ビットで読む
 * (高速バルクは 512)。 */
int ehci_parse_config(const uint8_t *cfg, size_t len,
                      ehci_device *info);

#endif /* PRISM_EHCI_H */
