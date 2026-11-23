/* PrismOS — 互換レイヤー (他の OS 向けの実行ファイルを動かす)
 *
 * 対象は 64 ビット ELF 実行ファイル (ET_EXEC / ET_DYN)。
 * ローダーはヘッダとプログラムヘッダを解析し、PT_LOAD を
 * 載せる。動的リンク (PT_DYNAMIC) は小規模なゲスト向けに
 * 対応する: DT_NEEDED の共有ライブラリを VFS のファイル
 * から載せ、RELA 再配置 (RELATIVE / GLOB_DAT / JUMP_SLOT /
 * 64) を読み込み時にすべて解決し (遅延束縛はしない)、
 * ライブラリと本体の初期化関数 (DT_INIT / DT_INIT_ARRAY)
 * を入口の前に呼ぶ。対象外: TLS 再配置、COPY 再配置、
 * REL (加算なし) 形式、シンボルのバージョン管理、
 * dlopen 相当、実物の libc のような大規模ライブラリ
 * (初期化子がこの層の呼び出し範囲を超えるため)。
 *
 * 呼び出しの橋渡しは int $0x80 で行う。ゲストは eax に番号、
 * rdi/rsi/rdx/r10/r8/r9 に引数を入れて int $0x80 を呼ぶ。
 * 番号と対応表は下の COMPAT_SYS_*。対応していない番号は
 * -ENOSYS を返し、ログに残す (嘘の成功はしない)。
 *
 * このファイルは純粋ロジックで、カーネルとホストテストで
 * 同じソースを使う。入出力は compat_io のフック経由で、
 * カーネル側はシリアルと画面、ホストテスト側はバッファを
 * 渡す。実際に走らせる部分 (スタックの張り替えと int 0x80
 * の入口) はカーネル側のアセンブリ (compat_gate.S) にある。
 *
 * 隔離 (サンドボックス): ゲストはカーネル側のランナー
 * (kernel/compat_run.c + kernel/sandbox.c) が用意する
 * ゲスト専用のページテーブルでリング 3 で動く。ゲストの
 * メモリはイメージ・スタック・brk だけに限られ、カーネル
 * のメモリを触るとページフォルトでゲストだけが終わる。
 * 暴走したゲストはウォッチドッグが止める。ポインタ引数は
 * ここ (compat_ptr_ok) でも検証する (二重の守り)。
 * グラフィクスはゲスト専用の窓 (640x480) への矩形・
 * 画像・文字列描画、ネットワークは TCP クライアント
 * (接続・送受信・名前解決) に対応する。GUI ウィジェット
 * やサーバー側ソケットは未対応。カーネル内蔵アプリ自体
 * の隔離は無い (それらはカーネルの一部として動く)。
 */
#ifndef PRISM_COMPAT_H
#define PRISM_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/* ---- 呼び出し番号 (このレイヤー独自の対応表) ---- */
#define COMPAT_SYS_EXIT 0
#define COMPAT_SYS_WRITE 1
#define COMPAT_SYS_READ 2
#define COMPAT_SYS_OPEN 3
#define COMPAT_SYS_CLOSE 4
#define COMPAT_SYS_FSTAT 5
#define COMPAT_SYS_BRK 6
#define COMPAT_SYS_GETPID 7
#define COMPAT_SYS_LSEEK 8
/* グラフィクス: ゲスト専用の窓 (COMPAT_FB_WIDTH x
 * COMPAT_FB_HEIGHT) への描画。座標は窓の内側 (0,0) 起点。
 * 描いた内容は FB_FLUSH で画面に出る。 */
#define COMPAT_SYS_FB_INFO 9    /* (ptr {u32 w, u32 h}) -> 0 */
#define COMPAT_SYS_FB_FILL 10   /* (x, y, w, h, color=r8) */
#define COMPAT_SYS_FB_BLIT 11   /* (x, y, w, h, ptr=r8: u32[]) */
#define COMPAT_SYS_FB_TEXT 12   /* (x, y, ptr str, color=r10) */
#define COMPAT_SYS_FB_FLUSH 13  /* () -> 0 */
/* ネットワーク: TCP クライアント (ホスト名解決つき)。
 * ハンドルはファイルとは別の名前空間 (0 始まり)。 */
#define COMPAT_SYS_NET_RESOLVE 14 /* (ptr host, ptr out ip4) */
#define COMPAT_SYS_NET_CONNECT 15 /* (ptr host, port) -> handle */
#define COMPAT_SYS_NET_SEND 16    /* (handle, ptr, len) -> sent */
#define COMPAT_SYS_NET_RECV 17    /* (handle, ptr, cap) -> n */
#define COMPAT_SYS_NET_CLOSE 18   /* (handle) -> 0 */

#define COMPAT_FB_WIDTH 640
#define COMPAT_FB_HEIGHT 480
#define COMPAT_NET_MAX_STREAMS 2

/* ---- lseek の起点 ---- */
#define COMPAT_SEEK_SET 0
#define COMPAT_SEEK_CUR 1
#define COMPAT_SEEK_END 2

/* ---- 失敗の番号 (負の値で返す) ---- */
#define COMPAT_ENOENT 2
#define COMPAT_EBADF 9
#define COMPAT_EFAULT 14
#define COMPAT_EACCES 13
#define COMPAT_EINVAL 22
#define COMPAT_EMFILE 24
#define COMPAT_ENOSPC 28
#define COMPAT_ENOSYS 38

/* ---- open のフラグ (ゲスト側) ---- */
#define COMPAT_O_RDONLY 0
#define COMPAT_O_WRONLY 1
#define COMPAT_O_RDWR 2
#define COMPAT_O_CREAT 0x40
#define COMPAT_O_TRUNC 0x200

#define COMPAT_MAX_SEGMENTS 16
#define COMPAT_MAX_FDS 8
#define COMPAT_BRK_SIZE (64 * 1024)
#define COMPAT_STACK_SIZE (16 * 1024)
#define COMPAT_WRITE_BUF_MAX 4096

/* PT_LOAD 1 個分。 */
typedef struct {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;
} compat_segment;

#define COMPAT_MAX_NEEDED 4
#define COMPAT_NEEDED_NAME_LEN 32

/* 動的リンクの情報 (PT_DYNAMIC があるとき)。アドレス系は
 * ファイル内オフセットに直してある (DT_INIT と初期化配列の
 * 先頭だけは仮想アドレスも持つ: 呼ぶときに要るため)。 */
typedef struct {
    int present;
    int needed_count;
    char needed[COMPAT_MAX_NEEDED][COMPAT_NEEDED_NAME_LEN];
    uint64_t symtab_off;
    uint64_t strtab_off;
    uint64_t strsz;
    uint32_t sym_count;
    uint64_t rela_off;
    uint64_t rela_sz;
    uint64_t rel_off;
    uint64_t rel_sz;
    uint64_t jmprel_off;
    uint64_t jmprel_sz;
    int pltrel_is_rela; /* 1 = RELA, 0 = REL */
    uint64_t init_vaddr; /* DT_INIT (0 = なし) */
    uint64_t init_array_vaddr;
    uint64_t init_array_off;
    uint64_t init_array_sz;
} compat_dynamic_info;

typedef struct {
    uint64_t entry;
    int is_exec; /* 1 = ET_EXEC, 0 = ET_DYN */
    int seg_count;
    compat_segment segs[COMPAT_MAX_SEGMENTS];
    uint64_t min_vaddr;
    uint64_t max_vaddr;
    compat_dynamic_info dyn;
} compat_elf_info;

/* ELF の解析。0 = 成功、負 = 理由 (compat_elf_error で文にする)。 */
int compat_elf_parse(const uint8_t *data, size_t len, compat_elf_info *out);
const char *compat_elf_error(int rc);

/* 呼び出しの対応表。 */
const char *compat_syscall_name(uint64_t nr);
int compat_syscall_supported(uint64_t nr);

/* ゲストのレジスタ (int 0x80 の入口が積む順番と一致する)。 */
typedef struct {
    uint64_t rax, rcx, rdx, rbx, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
} compat_regs;

/* 拡張フック: グラフィクスとネットワーク。カーネル側の
 * ランナーが実物 (フレームバッファと netstack) を渡す。
 * NULL の呼び出しは -ENOSYS になる (嘘の成功はしない)。
 * ポインタ引数はゲストのアドレスのまま渡る (呼び出し側が
 * compat_ptr_ok で検証済み)。 */
typedef struct {
    int (*fb_fill)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t rgb);
    int (*fb_blit)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const uint32_t *pixels);
    int (*fb_text)(uint32_t x, uint32_t y, const char *s,
                   uint32_t rgb);
    int (*fb_flush)(void);
    int (*net_resolve)(const char *host, uint8_t ip_out[4]);
    int (*net_connect)(const char *host, uint16_t port);
    int (*net_send)(int handle, const uint8_t *data, size_t len);
    int (*net_recv)(int handle, uint8_t *buf, size_t cap);
    int (*net_close)(int handle);
} compat_ext_io;

/* 入出力のフック。 */
typedef struct {
    void (*write_out)(const char *data, size_t len);
    int (*read_in)(char *buf, size_t len);
    void (*log_unsupported)(uint64_t nr);
    const compat_ext_io *ext; /* 任意 (NULL 可) */
} compat_io;

typedef void *(*compat_alloc_fn)(size_t size);
typedef void (*compat_free_fn)(void *ptr);

/* 載せたイメージ: セグメントのコピーと brk の領域。 */
typedef struct {
    uint8_t *seg_base[COMPAT_MAX_SEGMENTS];
    uint64_t seg_vaddr[COMPAT_MAX_SEGMENTS];
    uint64_t seg_memsz[COMPAT_MAX_SEGMENTS];
    int seg_count;
    uint64_t entry; /* 載せ直した後の入口アドレス */
    uint64_t base;  /* いちばん低いセグメントのアドレス */
    uint8_t *brk_base;
    uint64_t brk_cur;
} compat_image;

int compat_load_image(const compat_elf_info *info, const uint8_t *data,
                      size_t len, compat_alloc_fn alloc, compat_image *out);
void compat_free_image(compat_image *img, compat_free_fn free_fn);

/* ゲストのポインタ検証: ランナーが有効な範囲を登録する。 */
void compat_set_ranges(const compat_image *img, const uint8_t *stack_base,
                       size_t stack_size);
void compat_clear_ranges(void);
int compat_ptr_ok(uint64_t ptr, size_t len);

/* 呼び出しの状態: 開いているファイルと終了の合図。 */
void compat_reset_state(void);
extern int compat_exit_pending;
extern int compat_exit_code;

/* 呼び出しの入口が呼ぶ C の関数。フックとイメージは
 * compat_set_context で先に渡しておく。 */
void compat_set_context(const compat_io *io, compat_image *img,
                        compat_alloc_fn alloc, compat_free_fn free_fn);
int64_t compat_syscall_dispatch_c(compat_regs *r);

/* カーネル側のアセンブリ (compat_gate.S) が用意する。
 * entry に stack_top のスタックでジャンプする (本物の
 * プロセス起動と同じく、rsp は argc を指す)。ゲストが
 * exit の呼び出しで終わると、終了コードを返す。入口から
 * 戻ることは想定しない (ゲストは exit で終わること)。 */
int compat_call_entry(uint64_t entry, uint64_t stack_top);

#endif /* PRISM_COMPAT_H */
