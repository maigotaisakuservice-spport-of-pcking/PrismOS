/* PrismOS — PE (exe) ローダーとコンソール系 Win32 サブセット。
 *
 * 対象は 64 ビット PE (PE32+、x86-64) のコンソール
 * アプリケーション。パーサー (pe_parse) は純粋ロジックで、
 * セクション・インポート・再配置ディレクトリを読む。
 * 実際に載せて走らせるのはカーネル側のランナー
 * (kernel/compat_run.c) で、ELF ゲストと同じサンドボックス
 * (リング 3 + 専用ページテーブル) を使う。
 *
 * インポートは、ゲスト内に置いた小さなスタブ (int $0x80
 * の Win32 呼び出し番号 0x1000+連番) へ解決する。対応する
 * API は KERNEL32.dll のコンソール系サブセットのみで、
 * 対応表に無いインポートは読み込み自体を失敗させる
 * (知らない API を成功に見せかけない)。
 *
 * 正直な制限:
 *   - ヒープ (HeapAlloc / VirtualAlloc) はイメージ直後の
 *     brk 領域からの切り出しで、解放はプロセス存続期間中
 *     の no-op (HeapFree / VirtualFree は成功を返すだけ)。
 *   - TLS コールバック、例外処理 (SEH)、スレッド、GUI
 *     (USER32/GDI32) は未対応。DLL は KERNEL32.dll のみ。
 *   - 引数の 5 番目以降はゲストスタックから読む
 *     (compat_gate.S のレジスタ保存並びに依存する)。
 */
#ifndef PRISM_PE_H
#define PRISM_PE_H

#include <stddef.h>
#include <stdint.h>

#include "compat.h"

#define PE_MAX_SECTIONS 16
#define PE_MAX_IMPORTS 64
#define PE_IMPORT_NAME_LEN 64
#define PE_DLL_NAME_LEN 32

typedef struct {
    char name[9];
    uint32_t vsize;
    uint32_t vaddr; /* RVA */
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t chars;
} pe_section;

typedef struct {
    char dll[PE_DLL_NAME_LEN];
    char func[PE_IMPORT_NAME_LEN]; /* by_ordinal のとき空 */
    int by_ordinal;
    uint16_t ordinal;
    uint32_t iat_rva; /* この関数の IAT スロットの RVA */
} pe_import;

typedef struct {
    uint64_t image_base;
    uint64_t entry_rva;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint16_t subsystem; /* 2 = GUI, 3 = コンソール */
    int section_count;
    pe_section sections[PE_MAX_SECTIONS];
    int import_count;
    pe_import imports[PE_MAX_IMPORTS];
    uint32_t reloc_rva;
    uint32_t reloc_size;
} pe_info;

/* PE を解析する。0 = 成功、負 = 理由 (pe_error で文に)。 */
int pe_parse(const uint8_t *data, size_t len, pe_info *out);
const char *pe_error(int rc);

/* RVA をファイル内オフセットへ直す。範囲外は -1。 */
int pe_rva_to_offset(const pe_info *info, const uint8_t *data,
                     size_t len, uint32_t rva, uint32_t *out_off);

/* このインポートが対応表にあるか (名前で判定)。 */
int pe_import_supported(const pe_import *imp);

/* ランナーが渡す文脈。sleep/tick は任意 (NULL なら
 * Sleep は即時復帰、GetTickCount は 0)。cmdline_va は
 * GetCommandLineA が返すゲスト側アドレス (0 なら
 * 空文字列のアドレスとして 0 を返す)。 */
typedef struct {
    const compat_io *io;
    void (*sleep_ms)(uint32_t ms);
    uint32_t (*tick_ms)(void);
    uint64_t cmdline_va;
} pe_io;

void pe_set_context(const pe_info *info, const pe_io *io,
                    compat_image *img);
void pe_clear_context(void);

/* Win32 呼び出しの振り分け。index はインポート連番。
 * compat_syscall_dispatch_c から呼ばれる。 */
int64_t pe_win32_dispatch(uint32_t index, compat_regs *r);

#endif /* PRISM_PE_H */
