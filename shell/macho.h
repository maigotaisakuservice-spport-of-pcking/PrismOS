/* PrismOS — Mach-O (64 ビット) ローダー。
 *
 * 対象は 64 ビット Mach-O (MH_MAGIC_64、x86-64) の
 * コンソールアプリケーション。ヘッダとロードコマンドを
 * 解析し、LC_SEGMENT_64 のセグメントを載せ、LC_MAIN
 * (無ければ LC_UNIXTHREAD) の入口から走らせる。
 * 実際に載せて走らせるのはカーネル側のランナー
 * (kernel/compat_run.c) で、ELF ゲストと同じサンドボックス
 * (リング 3 + 専用ページテーブル) を使う。
 * ゲストは PrismOS の互換呼び出し (int $0x80) を使う。
 *
 * 正直な制限:
 *   - 動的ライブラリ (dyld) を要するものは未対応。
 *     LC_LOAD_DYLIB / LC_LOAD_DYLINKER などがあれば
 *     読み込み自体を失敗させる (実物の macOS バイナリは
 *     ほぼすべて dyld を要するため、動くのは自前の
 *     静的なゲストに限る)。
 *   - 32 ビット Mach-O、ARM 向け Mach-O は対象外。
 *   - .dmg はディスクイメージであり実行形式ではないので
 *     対象外。マウントもしない。.app はバンドル内の
 *     Mach-O 実行ファイル (Contents/MacOS/<名前>) を
 *     対象とする。
 */
#ifndef PRISM_MACHO_H
#define PRISM_MACHO_H

#include <stddef.h>
#include <stdint.h>

#define MACHO_MAX_SEGMENTS 8
#define MACHO_MAX_COMMANDS 64

typedef struct {
    char segname[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
} macho_segment;

typedef struct {
    uint64_t entry; /* 入口の仮想アドレス */
    uint64_t image_base; /* 最も低いセグメントの vmaddr */
    int seg_count;
    macho_segment segs[MACHO_MAX_SEGMENTS];
    int has_main; /* LC_MAIN があった */
} macho_info;

/* Mach-O を解析する。0 = 成功、負 = 理由 (macho_error で文に)。 */
int macho_parse(const uint8_t *data, size_t len, macho_info *out);
const char *macho_error(int rc);

#endif /* PRISM_MACHO_H */
