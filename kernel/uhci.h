/* PrismOS — UHCI USB ホストコントローラ
 *
 * QEMU の piix3 UHCI (PCI クラス 0C/03/00) を初期化し、
 * ポートに繋がった機器を列挙する。列挙時に構成を設定し
 * (SET_CONFIGURATION)、種類ごとにドライバを動かす:
 *
 * - HID キーボード / マウス (ブートプロトコル): 割り込み
 *   転送でレポートを受け、PS/2 と同じ入力キュー・マウス
 *   合流先へ流す (uhci_input_poll)。
 * - USB マスストレージ (Bulk-Only Transport + SCSI):
 *   INQUIRY / READ CAPACITY / READ(10) / WRITE(10) を実装し、
 *   ブロック層の第 2 ディスクとして登録する。
 *
 * ハブ経由の機器も 1 段まで対応する (ハブの
 * ポート電源・リセットを制御して下流を列挙する)。
 *
 * 流れ: フレーム一覧 (1024 項目) と転送記述子 (TD) を
 * PMM から確保した物理ページに置き、スケジュールを動かす。
 * ポートをリセットして有効にし、制御転送で機器記述子と
 * 構成記述子を読み、売主・製品・種別を表に記録する。
 *
 * 純粋な組み立て関数 (uhci_build_setup / uhci_td_token /
 * uhci_parse_device_desc) はハードウェアに触らないので、
 * ホスト側のユニットテストでも同じソースを動かせる。
 */
#ifndef PRISM_UHCI_H
#define PRISM_UHCI_H

#include <stddef.h>
#include <stdint.h>

#define UHCI_MAX_DEVICES 8

/* 機器のドライバ種別。 */
#define UHCI_KIND_UNKNOWN 0
#define UHCI_KIND_HID_KEYBOARD 1
#define UHCI_KIND_HID_MOUSE 2
#define UHCI_KIND_STORAGE 3

/* 列挙した 1 台の情報。 */
typedef struct {
    uint8_t address;    /* 割り当てた USB アドレス */
    uint8_t via_hub;    /* ハブ経由なら 1 */
    uint8_t port;       /* 繋がっているポート (1 または 2) */
    uint16_t vendor;    /* 売主 ID */
    uint16_t product;   /* 製品 ID */
    uint8_t dev_class;  /* 機器の種別 */
    uint8_t if_class;   /* 最初の接続面の種別 (0 = 不明) */
    uint8_t max_packet; /* 制御転送の最大パケット */
    int low_speed;      /* 低速機器なら 1 */
    /* ここから構成設定後に埋まる欄。 */
    uint8_t kind;         /* UHCI_KIND_* */
    uint8_t config_value; /* 設定した構成番号 */
    uint8_t if_num;       /* ドライバが使う接続面の番号 */
    uint8_t hid_ep;       /* HID 割り込み IN の終端番号 */
    uint8_t hid_max_packet;
    uint8_t bulk_in_ep;   /* ストレージ用バルク終端 */
    uint8_t bulk_out_ep;
    uint8_t bulk_max_packet;
    uint32_t storage_sectors; /* 512 バイト単位の容量 */
} uhci_device;

/* UHCI を探して初期化し、機器を列挙する。HHDM のずれを渡す。
 * 見つかれば 1、無ければ 0 を返す。何度呼んでもよい。 */
int uhci_init(uint64_t hhdm_offset);

/* コントローラが見つかったかどうか。uhci_init() の後で有効。 */
int uhci_present(void);

/* 列挙した機器の数。 */
int uhci_device_count(void);

/* i 番目の機器の情報。範囲外なら NULL。 */
const uhci_device *uhci_device_at(int i);

/* ---- 純粋関数 (ホストテスト用) ---- */

/* 制御転送の設定パケット (8 バイト) を作る。pkt は 8 バイト
 * 以上あること。 */
void uhci_build_setup(uint8_t *pkt, uint8_t req_type, uint8_t request,
                      uint16_t value, uint16_t index, uint16_t length);

/* 転送記述子のトークン (32 ビット) を作る。pid は 0x2D
 * (SETUP) / 0x69 (IN) / 0xE1 (OUT)。addr は機器アドレス、
 * ep は終端番号、toggle は 0/1、maxlen は転送バイト数。 */
uint32_t uhci_td_token(uint32_t pid, uint8_t addr, uint8_t ep, int toggle,
                       uint32_t maxlen);

/* 機器記述子 (18 バイト) から情報を抜く。info は 0 で埋めて
 * から各欄を入れる。 */
void uhci_parse_device_desc(const uint8_t *d, uhci_device *info);

/* TD の状態が「完了して誤りなし」かどうか。1 = 正常完了。 */
int uhci_td_ok(uint32_t status);

/* 構成記述子 (全部) を歩いて、構成番号と最初の HID /
 * ストレージ接続面の終端情報を info に入れる。成功 0。 */
int uhci_parse_config(const uint8_t *cfg, size_t len, uhci_device *info);

/* HID 使用コードを、PS/2 側と同じキーコードへ変換する。
 * shift / ctrl は修飾キーの状態。対応が無ければ -1。 */
int uhci_hid_key_code(uint8_t usage, int shift, int ctrl);

/* マスストレージの CBW (31 バイト) を組み立てる。 */
void uhci_build_cbw(uint8_t *cbw, uint32_t tag, uint32_t data_len,
                    int data_in, const uint8_t *cdb, size_t cdb_len);

/* HID レポートを取りに行き、キーボード / マウス入力へ
 * 流す。シェルとデスクトップの入力待ちから呼ばれる。 */
void uhci_input_poll(void);

/* USB マスストレージがあるかどうか (列挙 + INQUIRY +
 * READ CAPACITY まで成功したもの)。 */
int uhci_storage_present(void);

/* ストレージの容量 (512 バイトセクタ数)。 */
uint32_t uhci_storage_sectors(void);

/* ストレージの 1 セクタ読み書き。成功 0。 */
int uhci_storage_read_sector(uint32_t lba, uint8_t *buf);
int uhci_storage_write_sector(uint32_t lba, const uint8_t *buf);

#endif /* PRISM_UHCI_H */
