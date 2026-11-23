/* PrismOS — xHCI USB 3.x ホストコントローラ
 *
 * PCI クラス 0C/03/30 (xHCI) を初期化し、ルートハブの
 * ポートに繋がった機器を列挙する。コマンドリング /
 * イベントリング / 転送リングをポーリングで回す実装。
 * 対応する実動作はマスストレージ (Bulk-Only Transport +
 * SCSI) で、ブロック層へ追加ディスク (デバイス 5) として
 * 登録する。HID は UHCI / EHCI 側が担当する。
 *
 * 純粋な組み立て関数 (xhci_ep_info2 / xhci_slot_info /
 * xhci_parse_config など) はハードウェアに触らないので、
 * ホスト側のユニットテストでも同じソースを動かせる。
 */
#ifndef PRISM_XHCI_H
#define PRISM_XHCI_H

#include <stddef.h>
#include <stdint.h>

#define XHCI_MAX_DEVICES 4

/* 列挙した 1 台の情報 (EHCI 版と同じ形 + xHCI 固有)。 */
typedef struct {
    uint8_t address;    /* Address Device で割り当てられた番地 */
    uint8_t port;       /* 繋がっているポート (1 始まり) */
    uint16_t vendor;    /* 売主 ID */
    uint16_t product;   /* 製品 ID */
    uint8_t dev_class;  /* 機器の種別 */
    uint8_t if_class;   /* 最初の接続面の種別 (0 = 不明) */
    uint8_t max_packet; /* 制御転送の最大パケット */
    int speed;          /* 0=全速、1=低速、2=高速、3=超速 */
    /* ここから構成設定後に埋まる欄。 */
    uint8_t if_num;       /* ストレージ接続面の番号 */
    uint8_t bulk_in_ep;   /* ストレージ用バルク終端 */
    uint8_t bulk_out_ep;
    uint16_t bulk_max_packet;
    uint32_t storage_sectors; /* 512 バイト単位の容量 */
    /* xHCI 固有。 */
    uint8_t slot;         /* 割り当てられたスロット ID */
    uint8_t bulk_in_dci;  /* バルク IN の機器文脈番号 */
    uint8_t bulk_out_dci; /* バルク OUT の機器文脈番号 */
} xhci_device;

/* xHCI を探して初期化し、機器を列挙する。HHDM のずれを
 * 渡す。見つかれば 1、無ければ 0 を返す。 */
int xhci_init(uint64_t hhdm_offset);

/* コントローラが見つかったかどうか。 */
int xhci_present(void);

/* 列挙した機器の数。 */
int xhci_device_count(void);

/* i 番目の機器の情報。範囲外なら NULL。 */
const xhci_device *xhci_device_at(int i);

/* USB マスストレージがあるかどうか (列挙 + Configure
 * Endpoint + INQUIRY + READ CAPACITY まで成功したもの)。 */
int xhci_storage_present(void);

/* ストレージの容量 (512 バイトセクタ数)。 */
uint32_t xhci_storage_sectors(void);

/* ストレージの 1 セクタ読み書き。成功 0。 */
int xhci_storage_read_sector(uint32_t lba, uint8_t *buf);
int xhci_storage_write_sector(uint32_t lba, const uint8_t *buf);

/* ---- 純粋関数 (ホストテスト用) ---- */

/* 終端文脈の第 2 ワード (ep_info2) を作る。ep_type は
 * 1=等時 OUT / 2=バルク OUT / 3=割込 OUT / 4=制御 /
 * 5=等時 IN / 6=バルク IN / 7=割込 IN。cerr はエラー
 * 計数 (通常 3)。 */
uint32_t xhci_ep_info2(int ep_type, uint16_t max_packet,
                       uint8_t max_burst, int cerr);

/* スロット文脈の第 1 ワードを作る。speed はポート状態の
 * 速度符号 (1=全速 / 2=低速 / 3=高速 / 4=超速)、
 * ctx_entries は有効な終端文脈の最大番号。 */
uint32_t xhci_slot_info(int speed, int ctx_entries);

/* 制御転送の Setup TRB の転送種別 (TRT) を返す。
 * データなし 0、OUT データ 2、IN データ 3。 */
uint32_t xhci_setup_trt(int dir_in, int has_data);

/* TRB の制御ワードを作る。type は TRB 種別、cycle は
 * サイクルビット、flags は IOC / IDT / DIR などの
 * 既にシフト済みのビット。 */
uint32_t xhci_trb_control(int type, int cycle, uint32_t flags);

/* 構成記述子 (全部) を歩いて、マスストレージ接続面の
 * 終端情報を info に入れる。見つかれば 0。 */
int xhci_parse_config(const uint8_t *cfg, size_t len,
                      xhci_device *info);

#endif /* PRISM_XHCI_H */
