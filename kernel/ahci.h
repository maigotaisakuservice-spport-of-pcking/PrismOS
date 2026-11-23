/* PrismOS — AHCI SATA ディスクドライバ
 *
 * QEMU の q35 構成の AHCI コントローラ (PCI クラス 01/06)
 * から SATA ディスクを読み書きする。割り込みは使わず、
 * コマンド完了をポーリングする。実装ポートの中から SATA
 * ディスクが繋がった最初のポートを 1 つ使う (CD は使わない)。
 * 1 コマンドは 1 セクタ (512 バイト)。LBA48 の DMA 読み書き
 * (READ DMA EXT / WRITE DMA EXT) を使う。
 *
 * コマンド一覧・受信 FIS・コマンド表は PMM から確保した
 * 物理ページに置き、デバイスには物理アドレスを渡す。
 * カーネル側は HHDM 経由の仮想アドレスで触る。
 *
 * 純粋な組み立て関数 (ahci_build_reg_fis / ahci_prdt_entry /
 * ahci_parse_identify) はハードウェアに触らないので、
 * ホスト側のユニットテストでも同じソースを動かせる。
 */
#ifndef PRISM_AHCI_H
#define PRISM_AHCI_H

#include <stddef.h>
#include <stdint.h>

#define AHCI_SECTOR_SIZE 512

/* AHCI コントローラを探して初期化する。HHDM のずれを渡す。
 * 見つかれば 1、無ければ 0 を返す。何度呼んでもよい。 */
int ahci_init(uint64_t hhdm_offset);

/* ディスクが見つかったかどうか。ahci_init() の後で有効。 */
int ahci_present(void);

/* ディスクの総セクタ数。無い場合は 0。 */
uint64_t ahci_total_sectors(void);

/* IDENTIFY で読んだモデル名 (NUL 終端、最大 40 文字)。
 * 無い場合は空文字列。 */
const char *ahci_model(void);

/* 1 セクタ読む。成功したら 0、失敗したら -1。
 * buf は AHCI_SECTOR_SIZE バイト以上あること。 */
int ahci_read_sector(uint64_t lba, uint8_t *buf);

/* 1 セクタ書く。成功したら 0、失敗したら -1。
 * buf は AHCI_SECTOR_SIZE バイト以上あること。 */
int ahci_write_sector(uint64_t lba, const uint8_t *buf);

/* ---- 純粋関数 (ホストテスト用) ---- */

/* コマンド一覧の項目 0 の DW0 を作る。CFL=5、書き込みなら
 * W ビット、PRDTL は DW0 のビット 31:16 に入る。
 * prdtl は PRDT 項目の数。 */
uint32_t ahci_cmd_header_flags(int write, uint16_t prdtl);

/* Register Host-to-Device FIS (20 バイト) を作る。
 * cmd は ATA コマンド、lba は 48 ビットまで、count は
 * セクタ数。fis は 20 バイト以上あること。 */
void ahci_build_reg_fis(uint8_t *fis, uint8_t cmd, uint64_t lba,
                        uint16_t count);

/* PRDT 項目 (16 バイト) を作る。addr はデータの物理アドレス、
 * byte_count は転送バイト数 (1〜4194304)。entry は 16 バイト
 * 以上あること。 */
void ahci_prdt_entry(uint8_t *entry, uint64_t addr, uint32_t byte_count);

/* IDENTIFY の 256 ワードからモデル名と総セクタ数を抜く。
 * model は model_cap バイト以上。LBA48 の総セクタ数
 * (ワード 100-103) が 0 なら LBA28 (ワード 60-61) を使う。 */
void ahci_parse_identify(const uint16_t *id, char *model, size_t model_cap,
                         uint64_t *sectors);

#endif /* PRISM_AHCI_H */
