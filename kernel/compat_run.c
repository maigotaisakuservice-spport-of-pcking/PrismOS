/* PrismOS — 互換レイヤーのランナー (compat_run.h を見ること)。
 *
 * ゲストはサンドボックスで動かす (kernel/sandbox.c):
 * ゲスト専用のページテーブルに、イメージ・スタック・brk
 * だけをユーザーページとして張り、リング 3 で実行する。
 * カーネルのメモリは管理者専用のまま共有されるので、
 * ゲストが触るとページフォルトになり、ゲストだけが
 * 終わる。ポインタ引数の検証 (compat_ptr_ok) は従来通り
 * ディスパッチャ側でも行う (二重の守り)。
 */
#include "compat_run.h"

#include "../shell/compat.h"
#include "../shell/macho.h"
#include "../shell/pe.h"
#include "../shell/vfs.h"
#include "desktop.h"
#include "fb.h"
#include "heap.h"
#include "keyboard.h"
#include "netstack.h"
#include "pmm.h"
#include "rtc.h"
#include "sandbox.h"
#include "serial.h"
#include "string.h"

/* ゲストのスタックを張るアドレス (ユーザー空間の高い方)。
 * 典型的な ELF の配置 (0x400000 付近) とは重ならない。 */
#define GUEST_STACK_VA 0x00007F0000000000ULL
/* 初期化関数の戻り先スタブを置くページ (スタックのすぐ下)。
 * イメージ + brk はこのアドレス未満でなければならない。 */
#define GUEST_STUB_VA (GUEST_STACK_VA - 0x1000ULL)
/* ET_DYN (PIE) を載せる基準アドレス。 */
#define GUEST_DYN_BASE 0x400000ULL

static uint64_t g_hhdm_offset;
extern uint64_t compat_guest_cr3; /* compat_gate.S の .bss */

void compat_set_hhdm(uint64_t hhdm_offset) {
    g_hhdm_offset = hhdm_offset;
    sandbox_init(hhdm_offset);
}

int compat_guest_addr_in_range(uint64_t addr) {
    return compat_ptr_ok(addr, 1);
}

/* ---- ゲストの入出力 (シリアル + 画面) ---- */

static void compat_write_out(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_write_char(data[i]);
        desktop_console_putc(data[i]);
    }
    desktop_refresh_console();
}

/* パイプから渡された標準入力 (compat_set_stdin で設定)。
 * 残っている間はキーボード/シリアルより先にこちらを返す。 */
static const char *g_stdin_buf;
static size_t g_stdin_len;
static size_t g_stdin_pos;

void compat_set_stdin(const char *data, size_t len) {
    g_stdin_buf = data;
    g_stdin_len = len;
    g_stdin_pos = 0;
}

static int compat_read_in(char *buf, size_t len) {
    if (g_stdin_buf != NULL && g_stdin_pos < g_stdin_len) {
        size_t n = g_stdin_len - g_stdin_pos;
        if (n > len) {
            n = len;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = g_stdin_buf[g_stdin_pos + i];
        }
        g_stdin_pos += n;
        return (int)n;
    }
    size_t n = 0;
    while (n < len) {
        int c = keyboard_get_char();
        if (c < 0) {
            c = serial_try_read_char();
        }
        if (c < 0) {
            break;
        }
        buf[n++] = (char)c;
    }
    return (int)n;
}

static void print_u64_dec(uint64_t value) {
    char buf[21];
    int i = 0;
    if (value == 0) {
        serial_write_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

static void compat_log_unsupported(uint64_t nr) {
    serial_write("compat: syscall ");
    print_u64_dec(nr);
    serial_write_line(" is not supported (ENOSYS).");
}

/* ---- 拡張フック: グラフィクスとネットワーク ----
 *
 * グラフィクスはゲスト専用の窓 (タイトルバー + 640x480 の
 * 内側) をフレームバッファに描く。窓は最初の描画呼び出し
 * で現れ、FB_FLUSH で画面に反映される。ゲストが終わった
 * 後はデスクトップ側の再描画で上書きされる。
 * ネットワークは netstack の TCP ストリームを 2 本まで
 * ゲストに貸す。実行が終わるたびに全部閉じる。 */
#define GUEST_WIN_TITLE 24
#define GUEST_WIN_PAD 8

static int g_win_drawn;
static uint64_t g_win_x, g_win_y;

static void guest_win_prepare(void) {
    if (g_win_drawn || !fb_ready()) {
        return;
    }
    uint64_t fw = fb_width(), fh = fb_height();
    uint64_t ow = COMPAT_FB_WIDTH + GUEST_WIN_PAD * 2;
    uint64_t oh = COMPAT_FB_HEIGHT + GUEST_WIN_TITLE +
                  GUEST_WIN_PAD;
    g_win_x = (fw > ow) ? (fw - ow) / 2 : 0;
    g_win_y = (fh > oh) ? (fh - oh) / 2 : 0;
    fb_fill_rect(g_win_x, g_win_y, ow, oh, 0x202028);
    fb_fill_rect(g_win_x, g_win_y, ow, GUEST_WIN_TITLE,
                 0x2456A6);
    fb_draw_text(g_win_x + 10, g_win_y + 4, "Guest graphics",
                 0xFFFFFF, 0x2456A6, 2);
    fb_fill_rect(g_win_x + GUEST_WIN_PAD,
                 g_win_y + GUEST_WIN_TITLE, COMPAT_FB_WIDTH,
                 COMPAT_FB_HEIGHT, 0x000000);
    g_win_drawn = 1;
}

static uint64_t guest_cx(uint32_t x) {
    return g_win_x + GUEST_WIN_PAD + x;
}

static uint64_t guest_cy(uint32_t y) {
    return g_win_y + GUEST_WIN_TITLE + y;
}

static int ext_fb_fill(uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, uint32_t rgb) {
    if (!fb_ready()) {
        return -1;
    }
    guest_win_prepare();
    fb_fill_rect(guest_cx(x), guest_cy(y), w, h, rgb);
    return 0;
}

static int ext_fb_blit(uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint32_t *pixels) {
    if (!fb_ready()) {
        return -1;
    }
    guest_win_prepare();
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            fb_put_pixel(guest_cx(x + col), guest_cy(y + row),
                         pixels[row * w + col]);
        }
    }
    return 0;
}

static int ext_fb_text(uint32_t x, uint32_t y, const char *s,
                       uint32_t rgb) {
    if (!fb_ready()) {
        return -1;
    }
    guest_win_prepare();
    fb_draw_text(guest_cx(x), guest_cy(y), s, rgb, 0x000000, 2);
    return 0;
}

static int ext_fb_flush(void) {
    if (!fb_ready()) {
        return -1;
    }
    guest_win_prepare();
    fb_flush();
    return 0;
}

static net_tcp_stream g_streams[COMPAT_NET_MAX_STREAMS];
static int g_stream_used[COMPAT_NET_MAX_STREAMS];

static int ext_net_resolve(const char *host, uint8_t ip_out[4]) {
    if (net_ensure() != 0) {
        return -1;
    }
    return net_resolve(host, ip_out);
}

static int ext_net_connect(const char *host, uint16_t port) {
    if (net_ensure() != 0) {
        return -1;
    }
    for (int i = 0; i < COMPAT_NET_MAX_STREAMS; i++) {
        if (g_stream_used[i]) {
            continue;
        }
        if (net_tcp_open(host, port, &g_streams[i]) != 0) {
            return -1;
        }
        g_stream_used[i] = 1;
        return i;
    }
    return -1;
}

static int ext_net_send(int handle, const uint8_t *data,
                        size_t len) {
    if (handle < 0 || handle >= COMPAT_NET_MAX_STREAMS ||
        !g_stream_used[handle]) {
        return -1;
    }
    if (net_tcp_send(&g_streams[handle], (const char *)data,
                     len) != 0) {
        return -1;
    }
    return (int)len;
}

static int ext_net_recv(int handle, uint8_t *buf, size_t cap) {
    if (handle < 0 || handle >= COMPAT_NET_MAX_STREAMS ||
        !g_stream_used[handle]) {
        return -1;
    }
    return net_tcp_recv(&g_streams[handle], buf, cap, 5);
}

static int ext_net_close(int handle) {
    if (handle < 0 || handle >= COMPAT_NET_MAX_STREAMS ||
        !g_stream_used[handle]) {
        return -1;
    }
    net_tcp_close(&g_streams[handle]);
    g_stream_used[handle] = 0;
    return 0;
}

/* 実行の前後片付け: 窓を未描画に戻し、開いたままの
 * ストリームを全部閉じる。 */
static void compat_ext_reset(void) {
    g_win_drawn = 0;
    for (int i = 0; i < COMPAT_NET_MAX_STREAMS; i++) {
        if (g_stream_used[i]) {
            net_tcp_close(&g_streams[i]);
            g_stream_used[i] = 0;
        }
    }
}

static const compat_ext_io g_kernel_ext = {
    ext_fb_fill,  ext_fb_blit,   ext_fb_text,  ext_fb_flush,
    ext_net_resolve, ext_net_connect, ext_net_send, ext_net_recv,
    ext_net_close,
};

static const compat_io g_kernel_io = {
    compat_write_out,
    compat_read_in,
    compat_log_unsupported,
    &g_kernel_ext,
};

/* ---- スタック組み立て ---- */

/* argc/argv/envp/auxv をスタックに並べる。SysV の並び:
 * argc, argv[0..n-1], NULL, envp(NULL), auxv(AT_NULL)。
 * スタックは 16 バイトにそろえる (envp に NULL を 1 個
 * 余分に入れて調整する。空の envp は NULL が 2 個でも同じ)。
 * メモリへの書き込みは stack_base (カーネルから見える別名)
 * で行うが、積むポインタ値は ptr_bias を足したものにする
 * (ゲストから見えるアドレスにするため)。戻り値も
 * バイアス後の rsp。 */
static uint64_t build_stack(const char *name, const char *const *args,
                            int nargs, uint8_t *stack_base,
                            size_t stack_size, uint64_t ptr_bias) {
    uint8_t *sp = stack_base + stack_size;
    int total = nargs + 1; /* argv[0] = ファイル名 */
    if (total > 16) {
        total = 16;
    }
    uint64_t ptrs[16];
    for (int i = total - 1; i >= 0; i--) {
        const char *s = (i == 0) ? name : args[i - 1];
        size_t n = strlen(s) + 1;
        sp -= n;
        memcpy(sp, s, n);
        ptrs[i] = (uint64_t)sp + ptr_bias;
    }
    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);
    /* auxv: AT_NULL (type 0, value 0)。 */
    sp -= 16;
    *(uint64_t *)sp = 0;
    *(uint64_t *)(sp + 8) = 0;
    /* envp: NULL。total が偶数のときはもう 1 個 NULL を入れて
     * 16 バイトにそろえる。 */
    sp -= 8;
    *(uint64_t *)sp = 0;
    if ((total & 1) == 0) {
        sp -= 8;
        *(uint64_t *)sp = 0;
    }
    /* argv の終わりの NULL。 */
    sp -= 8;
    *(uint64_t *)sp = 0;
    for (int i = total - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = ptrs[i];
    }
    /* argc。 */
    sp -= 8;
    *(uint64_t *)sp = (uint64_t)total;
    return (uint64_t)sp + ptr_bias;
}

/* ---- サンドボックスへの載せ込み ---- */

/* ゲストのメモリへ書き込む (ページ単位で HHDM 経由に直す)。 */
static int guest_copy_in(uint64_t pml4, uint64_t dst_va,
                         const uint8_t *src, size_t len) {
    while (len > 0) {
        uint64_t phys = sandbox_lookup(pml4, dst_va & ~0xFFFULL);
        if (phys == 0) {
            return -1;
        }
        size_t off = (size_t)(dst_va & 0xFFF);
        size_t chunk = 4096 - off;
        if (chunk > len) {
            chunk = len;
        }
        memcpy((uint8_t *)(uintptr_t)(phys + g_hhdm_offset + off),
               src, chunk);
        dst_va += chunk;
        src += chunk;
        len -= chunk;
    }
    return 0;
}

/* ゲストのメモリから読み出す (guest_copy_in の逆)。 */
static int guest_copy_out(uint64_t pml4, uint64_t src_va,
                          uint8_t *dst, size_t len) {
    while (len > 0) {
        uint64_t phys = sandbox_lookup(pml4, src_va & ~0xFFFULL);
        if (phys == 0) {
            return -1;
        }
        size_t off = (size_t)(src_va & 0xFFF);
        size_t chunk = 4096 - off;
        if (chunk > len) {
            chunk = len;
        }
        memcpy(dst, (const uint8_t *)(uintptr_t)(phys +
                                                 g_hhdm_offset + off),
               chunk);
        src_va += chunk;
        dst += chunk;
        len -= chunk;
    }
    return 0;
}

static void img_init(compat_image *img) {
    for (int i = 0; i < COMPAT_MAX_SEGMENTS; i++) {
        img->seg_base[i] = NULL;
        img->seg_vaddr[i] = 0;
        img->seg_memsz[i] = 0;
    }
    img->seg_count = 0;
    img->entry = 0;
    img->base = 0;
    img->brk_base = NULL;
    img->brk_cur = 0;
}

/* 1 つのオブジェクトの PT_LOAD を pml4 に張り、img に積む。
 * bias は va = vaddr + bias の加算値。 */
static int map_segments(uint64_t pml4, const compat_elf_info *info,
                        const uint8_t *data, uint64_t bias,
                        compat_image *img) {
    for (int i = 0; i < info->seg_count; i++) {
        const compat_segment *s = &info->segs[i];
        uint64_t va = s->vaddr + bias;
        int writable = (s->flags & 2) != 0;   /* PF_W */
        int executable = (s->flags & 1) != 0; /* PF_X */
        uint64_t first = va & ~0xFFFULL;
        uint64_t last = (va + s->memsz + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t p = first; p < last; p += 4096) {
            if (sandbox_map_new(pml4, p, writable, executable,
                                NULL) != 0) {
                return -1;
            }
        }
        if (s->filesz > 0 &&
            guest_copy_in(pml4, va, data + s->offset,
                          (size_t)s->filesz) != 0) {
            return -1;
        }
        if (img != NULL) {
            if (img->seg_count >= COMPAT_MAX_SEGMENTS) {
                return -1;
            }
            int slot = img->seg_count++;
            img->seg_base[slot] = (uint8_t *)(uintptr_t)va;
            img->seg_vaddr[slot] = s->vaddr;
            img->seg_memsz[slot] = s->memsz;
            if (img->base == 0 || va < img->base) {
                img->base = va;
            }
        }
    }
    return 0;
}

/* ELF をゲストのページテーブルへ載せる。img の各フィールドは
 * ゲスト側のアドレス (ポインタ値) で埋める。成功したら 0、
 * *out_pml4 に PML4 の物理アドレス、*out_stack_phys に
 * スタックの物理アドレス (連続 4 ページ) を返す。 */
static int sandbox_load(const compat_elf_info *info,
                        const uint8_t *data, uint64_t *out_pml4,
                        uint64_t *out_stack_phys,
                        compat_image *img) {
    uint64_t reloc = 0;
    if (!info->is_exec) {
        reloc = GUEST_DYN_BASE - (info->min_vaddr & ~0xFFFULL);
    }
    uint64_t pml4 = sandbox_create();
    if (pml4 == 0) {
        return -1;
    }
    img_init(img);
    if (map_segments(pml4, info, data, reloc, img) != 0) {
        sandbox_destroy(pml4);
        return -1;
    }
    img->entry = info->entry + reloc;

    /* brk: イメージの直後 (ページ境界) に 64KB。 */
    uint64_t brk_va =
        (info->max_vaddr + reloc + 0xFFFULL) & ~0xFFFULL;
    if (brk_va + COMPAT_BRK_SIZE > GUEST_STUB_VA) {
        serial_write_line(
            "run: image collides with the guest stack area");
        sandbox_destroy(pml4);
        return -1;
    }
    for (uint64_t p = 0; p < COMPAT_BRK_SIZE; p += 4096) {
        if (sandbox_map_new(pml4, brk_va + p, 1, 0, NULL) != 0) {
            sandbox_destroy(pml4);
            return -1;
        }
    }
    img->brk_base = (uint8_t *)(uintptr_t)brk_va;
    img->brk_cur = brk_va;

    /* スタック: 連続した物理ページに張る (カーネル側から
     * HHDM 経由で連続に見える必要があるため)。 */
    uint64_t stack_phys =
        pmm_alloc_pages(COMPAT_STACK_SIZE / 4096);
    if (stack_phys == 0) {
        sandbox_destroy(pml4);
        return -1;
    }
    for (uint64_t p = 0; p < COMPAT_STACK_SIZE; p += 4096) {
        if (sandbox_map(pml4, GUEST_STACK_VA + p, stack_phys + p,
                        1, 0) != 0) {
            /* 張れた分は sandbox_destroy が解放する。張れて
             * いない残りだけをここで返す。 */
            pmm_free_pages(stack_phys + p,
                           (COMPAT_STACK_SIZE - p) / 4096);
            sandbox_destroy(pml4);
            return -1;
        }
    }
    *out_pml4 = pml4;
    *out_stack_phys = stack_phys;
    return 0;
}

/* ---- 動的リンク ---- */

/* ライブラリを載せる基準アドレス (本体は GUEST_DYN_BASE、
 * ライブラリはここから 16MB ずつずらす)。 */
#define GUEST_LIB_BASE 0x1000000ULL
#define GUEST_LIB_STRIDE 0x1000000ULL
#define DYN_MAX_OBJECTS (1 + COMPAT_MAX_NEEDED)

typedef struct {
    const uint8_t *data;
    compat_elf_info info;
    uint64_t bias; /* va = vaddr + bias */
    char name[COMPAT_NEEDED_NAME_LEN];
} dyn_object;

static dyn_object g_dyn_objs[DYN_MAX_OBJECTS];
static int g_dyn_count;

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

/* 名前から定義済みシンボルを探す (読み込み順 = 検索順)。
 * 見つかれば 1、無ければ 0。 */
static int dyn_lookup(const char *name, uint64_t *out_value) {
    for (int o = 0; o < g_dyn_count; o++) {
        const dyn_object *obj = &g_dyn_objs[o];
        const compat_dynamic_info *d = &obj->info.dyn;
        if (!d->present) {
            continue;
        }
        for (uint32_t i = 0; i < d->sym_count; i++) {
            const uint8_t *sym = obj->data + d->symtab_off +
                                 (uint64_t)i * 24;
            uint32_t st_name = rd32le(sym);
            uint16_t st_shndx = rd16le(sym + 6);
            if (st_shndx == 0 || st_name >= d->strsz) {
                continue; /* 未定義か壊れた名前 */
            }
            const char *sname =
                (const char *)obj->data + d->strtab_off + st_name;
            if (strcmp(sname, name) == 0) {
                *out_value = obj->bias + rd64le(sym + 8);
                return 1;
            }
        }
    }
    return 0;
}

/* 再配置 1 本分を処理する。未対応の型・見つからない
 * シンボルは理由を出して -1。 */
static int dyn_apply_rela(uint64_t pml4, const dyn_object *obj,
                          const uint8_t *rela, const char *obj_name) {
    uint64_t r_offset = rd64le(rela);
    uint64_t r_info = rd64le(rela + 8);
    int64_t r_addend = (int64_t)rd64le(rela + 16);
    uint32_t type = (uint32_t)(r_info & 0xFFFFFFFFULL);
    uint32_t symidx = (uint32_t)(r_info >> 32);
    const compat_dynamic_info *d = &obj->info.dyn;
    uint64_t value;
    switch (type) {
    case 0: /* R_X86_64_NONE */
        return 0;
    case 8: /* R_X86_64_RELATIVE */
        value = obj->bias + (uint64_t)r_addend;
        break;
    case 1:  /* R_X86_64_64 */
    case 6:  /* R_X86_64_GLOB_DAT */
    case 7: { /* R_X86_64_JUMP_SLOT */
        if (symidx >= d->sym_count) {
            serial_write_line("run: relocation symbol index out of range");
            return -1;
        }
        const uint8_t *sym = obj->data + d->symtab_off +
                             (uint64_t)symidx * 24;
        uint32_t st_name = rd32le(sym);
        uint8_t st_info = sym[4];
        if (st_name >= d->strsz) {
            serial_write_line("run: relocation symbol name out of range");
            return -1;
        }
        const char *name =
            (const char *)obj->data + d->strtab_off + st_name;
        uint64_t sv;
        if (dyn_lookup(name, &sv)) {
            value = sv + (uint64_t)r_addend;
        } else if ((st_info >> 4) == 2) {
            value = 0; /* 弱い未定義は 0 (仕様通り) */
        } else {
            serial_write("run: ");
            serial_write(obj_name);
            serial_write(": undefined symbol: ");
            serial_write_line(name);
            return -1;
        }
        break;
    }
    default:
        serial_write("run: ");
        serial_write(obj_name);
        serial_write(": unsupported relocation type ");
        print_u64_dec(type);
        serial_write_line("");
        return -1;
    }
    uint64_t dst = obj->bias + r_offset;
    if (dst < obj->bias + obj->info.min_vaddr ||
        dst + 8 > obj->bias + obj->info.max_vaddr) {
        serial_write_line("run: relocation target outside the object");
        return -1;
    }
    if (guest_copy_in(pml4, dst, (const uint8_t *)&value, 8) != 0) {
        serial_write_line("run: relocation write failed");
        return -1;
    }
    return 0;
}

static int dyn_relocate_object(uint64_t pml4,
                               const dyn_object *obj) {
    const compat_dynamic_info *d = &obj->info.dyn;
    if (d->rel_sz > 0) {
        serial_write("run: ");
        serial_write(obj->name);
        serial_write_line(": REL relocations are not supported");
        return -1;
    }
    for (uint64_t off = 0; off + 24 <= d->rela_sz; off += 24) {
        if (dyn_apply_rela(pml4, obj, obj->data + d->rela_off + off,
                           obj->name) != 0) {
            return -1;
        }
    }
    if (d->jmprel_sz > 0) {
        if (!d->pltrel_is_rela) {
            serial_write("run: ");
            serial_write(obj->name);
            serial_write_line(": REL PLT relocations are not supported");
            return -1;
        }
        for (uint64_t off = 0; off + 24 <= d->jmprel_sz; off += 24) {
            if (dyn_apply_rela(pml4, obj,
                               obj->data + d->jmprel_off + off,
                               obj->name) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* 初期化関数を 1 つ呼ぶ。スタックの先頭にスタブへの
 * 戻りアドレスを置くので、関数は普通に ret でき、戻ると
 * exit(0) が走って呼び出しが返る。 */
static int dyn_call_init(uint64_t pml4, uint64_t stack_phys,
                         uint64_t addr) {
    static const uint8_t stub[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax, 0 (exit) */
        0x31, 0xFF,                   /* xor edi, edi */
        0xCD, 0x80,                   /* int 0x80 */
        0xF4                          /* hlt */
    };
    if (guest_copy_in(pml4, GUEST_STUB_VA, stub, sizeof(stub)) != 0) {
        return -1;
    }
    uint8_t *stack_alias =
        (uint8_t *)(uintptr_t)(stack_phys + g_hhdm_offset);
    uint64_t stub_va = GUEST_STUB_VA;
    memcpy(stack_alias + COMPAT_STACK_SIZE - 8, &stub_va, 8);
    uint64_t sp_guest = GUEST_STACK_VA + COMPAT_STACK_SIZE - 8;
    compat_reset_state();
    compat_ext_reset();
    compat_guest_cr3 = pml4;
    compat_guest_active = 1;
    sandbox_watchdog_start();
    int code = compat_call_entry(addr, sp_guest);
    sandbox_watchdog_stop();
    compat_guest_active = 0;
    compat_guest_cr3 = 0;
    return (compat_exit_pending && code == 0) ? 0 : -1;
}

/* 本体と DT_NEEDED のライブラリを全部載せ、再配置まで
 * 済ませる。初期化関数の呼び出しは呼び出し側 (文脈を
 * 用意してから)。 */
static int sandbox_load_dynamic(const compat_elf_info *info,
                                const uint8_t *data,
                                const char *name, uint64_t *out_pml4,
                                uint64_t *out_stack_phys,
                                compat_image *img) {
    uint64_t pml4 = sandbox_create();
    if (pml4 == 0) {
        return -1;
    }
    img_init(img);
    g_dyn_count = 0;
    /* 本体。 */
    dyn_object *main_obj = &g_dyn_objs[0];
    main_obj->data = data;
    main_obj->info = *info;
    main_obj->bias = info->is_exec
                         ? 0
                         : GUEST_DYN_BASE - (info->min_vaddr & ~0xFFFULL);
    size_t nn = 0;
    while (name[nn] != '\0' && nn + 1 < sizeof(main_obj->name)) {
        main_obj->name[nn] = name[nn];
        nn++;
    }
    main_obj->name[nn] = '\0';
    g_dyn_count = 1;
    /* 依存を幅優先で集める。 */
    for (int o = 0; o < g_dyn_count; o++) {
        const compat_dynamic_info *d = &g_dyn_objs[o].info.dyn;
        for (int k = 0; k < d->needed_count; k++) {
            const char *need = d->needed[k];
            int already = 0;
            for (int q = 0; q < g_dyn_count; q++) {
                if (strcmp(g_dyn_objs[q].name, need) == 0) {
                    already = 1;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (g_dyn_count >= DYN_MAX_OBJECTS) {
                serial_write_line("run: too many libraries");
                sandbox_destroy(pml4);
                return -1;
            }
            size_t llen = 0;
            const char *ldata = vfs_read_ptr(need, &llen);
            if (ldata == NULL) {
                serial_write("run: ");
                serial_write(name);
                serial_write(": cannot open shared library: ");
                serial_write_line(need);
                sandbox_destroy(pml4);
                return -1;
            }
            dyn_object *lib = &g_dyn_objs[g_dyn_count];
            lib->data = (const uint8_t *)ldata;
            int rc = compat_elf_parse(lib->data, llen, &lib->info);
            if (rc != 0) {
                serial_write("run: ");
                serial_write(need);
                serial_write(": ");
                serial_write_line(compat_elf_error(rc));
                sandbox_destroy(pml4);
                return -1;
            }
            if (lib->info.is_exec) {
                serial_write("run: ");
                serial_write(need);
                serial_write_line(": not a shared library (ET_DYN required)");
                sandbox_destroy(pml4);
                return -1;
            }
            uint64_t base = GUEST_LIB_BASE +
                            (uint64_t)(g_dyn_count - 1) * GUEST_LIB_STRIDE;
            lib->bias = base - (lib->info.min_vaddr & ~0xFFFULL);
            nn = 0;
            while (need[nn] != '\0' && nn + 1 < sizeof(lib->name)) {
                lib->name[nn] = need[nn];
                nn++;
            }
            lib->name[nn] = '\0';
            g_dyn_count++;
        }
    }
    /* 全部のオブジェクトを同じページテーブルに張る。 */
    for (int o = 0; o < g_dyn_count; o++) {
        if (map_segments(pml4, &g_dyn_objs[o].info, g_dyn_objs[o].data,
                         g_dyn_objs[o].bias, img) != 0) {
            serial_write_line("run: out of memory (or too many segments)");
            sandbox_destroy(pml4);
            return -1;
        }
    }
    img->entry = info->entry + main_obj->bias;
    /* スタブのページ (実行のみ)。 */
    if (sandbox_map_new(pml4, GUEST_STUB_VA, 0, 1, NULL) != 0) {
        sandbox_destroy(pml4);
        return -1;
    }
    /* brk: 本体の直後 (ページ境界) に 64KB。 */
    uint64_t brk_va = (info->max_vaddr + main_obj->bias + 0xFFFULL) &
                      ~0xFFFULL;
    if (brk_va + COMPAT_BRK_SIZE > GUEST_STUB_VA) {
        serial_write_line(
            "run: image collides with the guest stack area");
        sandbox_destroy(pml4);
        return -1;
    }
    for (uint64_t p = 0; p < COMPAT_BRK_SIZE; p += 4096) {
        if (sandbox_map_new(pml4, brk_va + p, 1, 0, NULL) != 0) {
            sandbox_destroy(pml4);
            return -1;
        }
    }
    img->brk_base = (uint8_t *)(uintptr_t)brk_va;
    img->brk_cur = brk_va;
    /* スタック (sandbox_load と同じ)。 */
    uint64_t stack_phys = pmm_alloc_pages(COMPAT_STACK_SIZE / 4096);
    if (stack_phys == 0) {
        sandbox_destroy(pml4);
        return -1;
    }
    for (uint64_t p = 0; p < COMPAT_STACK_SIZE; p += 4096) {
        if (sandbox_map(pml4, GUEST_STACK_VA + p, stack_phys + p,
                        1, 0) != 0) {
            pmm_free_pages(stack_phys + p,
                           (COMPAT_STACK_SIZE - p) / 4096);
            sandbox_destroy(pml4);
            return -1;
        }
    }
    /* 再配置 (全部を読み込み時に解決する)。 */
    for (int o = 0; o < g_dyn_count; o++) {
        if (dyn_relocate_object(pml4, &g_dyn_objs[o]) != 0) {
            sandbox_destroy(pml4);
            return -1;
        }
    }
    *out_pml4 = pml4;
    *out_stack_phys = stack_phys;
    return 0;
}

/* ライブラリ → 本体の順に初期化関数を呼ぶ。 */
static int dyn_run_inits(uint64_t pml4, uint64_t stack_phys) {
    for (int o = g_dyn_count - 1; o >= 0; o--) {
        const dyn_object *obj = &g_dyn_objs[o];
        const compat_dynamic_info *d = &obj->info.dyn;
        if (d->init_vaddr != 0) {
            if (dyn_call_init(pml4, stack_phys,
                              obj->bias + d->init_vaddr) != 0) {
                serial_write("run: ");
                serial_write(obj->name);
                serial_write_line(": DT_INIT failed");
                return -1;
            }
        }
        for (uint64_t off = 0; off + 8 <= d->init_array_sz; off += 8) {
            uint64_t fn = 0;
            /* 再配置後の値をゲスト側から読む。 */
            if (guest_copy_out(pml4,
                               obj->bias + d->init_array_vaddr + off,
                               (uint8_t *)&fn, 8) != 0 ||
                fn == 0) {
                serial_write("run: ");
                serial_write(obj->name);
                serial_write_line(": bad init array entry");
                return -1;
            }
            if (dyn_call_init(pml4, stack_phys, fn) != 0) {
                serial_write("run: ");
                serial_write(obj->name);
                serial_write_line(": init function failed");
                return -1;
            }
        }
    }
    return 0;
}

/* ---- 実行 ---- */

/* ---- PE (exe) ゲスト ----
 *
 * ELF と同じサンドボックスに PE を載せる。インポートは
 * スタブページの小さなトランポリン (int $0x80 の Win32
 * 番号) へ解決し、対応表に無いものは実行前に断る。 */
#define WIN32_STUB_VA (GUEST_STUB_VA - 0x1000ULL)

static uint32_t pe_tick_ms(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    return rtc_to_unix(&t) * 1000u;
}

static uint64_t pe_unix_time(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    return (uint64_t)rtc_to_unix(&t);
}

static void pe_sleep_ms(uint32_t ms) {
    /* 概算のビジーウェイト (Sleep のための実用上の待ち)。
     * 秒単位は RTC で待ち、端数は pause ループで待つ。 */
    while (ms >= 1000) {
        uint32_t start = pe_tick_ms();
        while (pe_tick_ms() - start < 1000) {
            __asm__ volatile("pause");
        }
        ms -= 1000;
    }
    for (uint32_t i = 0; i < ms * 200000u; i++) {
        __asm__ volatile("pause");
    }
}

static int run_pe(const char *name, const uint8_t *data, size_t len,
                  const char *const *args, int nargs,
                  int *exit_code) {
    pe_info info;
    int rc = pe_parse(data, len, &info);
    if (rc != 0) {
        serial_write("run: ");
        serial_write(name);
        serial_write(": ");
        serial_write_line(pe_error(rc));
        return -2;
    }
    for (int i = 0; i < info.import_count; i++) {
        if (!pe_import_supported(&info.imports[i])) {
            serial_write("run: ");
            serial_write(name);
            serial_write(": unsupported import ");
            serial_write(info.imports[i].dll);
            serial_write("!");
            if (info.imports[i].by_ordinal) {
                serial_write("(ordinal)");
            } else {
                serial_write(info.imports[i].func);
            }
            serial_write_line("");
            return -2;
        }
    }
    /* 載せる場所: 希望ベースが空いていればそこ、駄目なら
     * 0x400000 へ再配置する (再配置表が無ければ断る)。 */
    uint64_t load = info.image_base;
    uint64_t bias = 0;
    int collide = load < 0x10000 ||
                  load + info.size_of_image > WIN32_STUB_VA;
    if (collide) {
        load = GUEST_DYN_BASE;
        bias = load - info.image_base;
        if (info.reloc_rva == 0) {
            serial_write_line(
                "run: image base collides and no .reloc present");
            return -2;
        }
    }
    uint64_t pml4 = sandbox_create();
    if (pml4 == 0) {
        return -3;
    }
    compat_image img;
    img_init(&img);
    img.base = load;
    img.entry = load + info.entry_rva;
    int fail = 0;
    /* ヘッダ。 */
    uint64_t hdr_end =
        (info.size_of_headers + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = 0; p < hdr_end && !fail; p += 4096) {
        fail = sandbox_map_new(pml4, load + p, 0, 0, NULL) != 0;
    }
    if (!fail && hdr_end > 0) {
        fail = guest_copy_in(pml4, load, data,
                             info.size_of_headers) != 0;
    }
    /* セクション。 */
    for (int i = 0; i < info.section_count && !fail; i++) {
        const pe_section *s = &info.sections[i];
        uint64_t va = load + s->vaddr;
        uint64_t span = s->vsize > s->raw_size ? s->vsize
                                               : s->raw_size;
        uint64_t first = va & ~0xFFFULL;
        uint64_t last = (va + span + 0xFFFULL) & ~0xFFFULL;
        int writable = (s->chars & 0x80000000u) != 0;
        int executable = (s->chars & 0x20000000u) != 0;
        for (uint64_t p = first; p < last && !fail; p += 4096) {
            fail = sandbox_map_new(pml4, p, writable, executable,
                                   NULL) != 0;
        }
        if (!fail && s->raw_size > 0) {
            fail = guest_copy_in(pml4, va, data + s->raw_ptr,
                                 s->raw_size) != 0;
        }
        if (!fail && img.seg_count < COMPAT_MAX_SEGMENTS) {
            int slot = img.seg_count++;
            img.seg_base[slot] = (uint8_t *)(uintptr_t)va;
            img.seg_vaddr[slot] = s->vaddr;
            img.seg_memsz[slot] = last - first;
        }
    }
    /* 再配置 (DIR64 のみ)。 */
    if (!fail && bias != 0) {
        uint32_t off = 0;
        if (pe_rva_to_offset(&info, data, len, info.reloc_rva,
                             &off) != 0) {
            fail = 1;
        }
        uint32_t done = 0;
        while (!fail && done < info.reloc_size) {
            uint32_t page_rva, block_size;
            if (off + done + 8 > len) {
                fail = 1;
                break;
            }
            memcpy(&page_rva, data + off + done, 4);
            memcpy(&block_size, data + off + done + 4, 4);
            if (block_size < 8 || done + block_size >
                                       info.reloc_size) {
                fail = 1;
                break;
            }
            int entries = (int)(block_size - 8) / 2;
            for (int e = 0; e < entries && !fail; e++) {
                uint16_t ent;
                memcpy(&ent, data + off + done + 8 +
                               (size_t)e * 2,
                       2);
                if ((ent >> 12) == 10) { /* DIR64 */
                    uint64_t va = load + page_rva +
                                  (ent & 0xFFF);
                    uint64_t val;
                    if (guest_copy_out(pml4, va,
                                       (uint8_t *)&val, 8) != 0) {
                        fail = 1;
                        break;
                    }
                    val += bias;
                    fail = guest_copy_in(pml4, va,
                                         (const uint8_t *)&val,
                                         8) != 0;
                }
            }
            done += block_size;
        }
    }
    /* brk 領域 (ヒープ用)。 */
    uint64_t brk_va =
        (load + info.size_of_image + 0xFFFULL) & ~0xFFFULL;
    if (!fail && brk_va + COMPAT_BRK_SIZE > WIN32_STUB_VA) {
        serial_write_line("run: image collides with the stub area");
        fail = 1;
    }
    for (uint64_t p = 0; p < COMPAT_BRK_SIZE && !fail; p += 4096) {
        fail = sandbox_map_new(pml4, brk_va + p, 1, 0, NULL) != 0;
    }
    if (!fail) {
        img.brk_base = (uint8_t *)(uintptr_t)brk_va;
        img.brk_cur = brk_va;
    }
    /* スタブページ 2 枚 (Win32 トランポリン + 終了スタブ)。 */
    if (!fail) {
        fail = sandbox_map_new(pml4, WIN32_STUB_VA, 0, 1, NULL) != 0 ||
               sandbox_map_new(pml4, GUEST_STUB_VA, 0, 1, NULL) != 0;
    }
    uint64_t cmdline_va = WIN32_STUB_VA + 2048;
    if (!fail) {
        for (int i = 0; i < info.import_count && !fail; i++) {
            uint8_t stub[16];
            uint32_t nr = 0x1000 + (uint32_t)i;
            memset(stub, 0, sizeof(stub));
            stub[0] = 0xB8; /* mov eax, nr */
            stub[1] = (uint8_t)(nr & 0xFF);
            stub[2] = (uint8_t)((nr >> 8) & 0xFF);
            stub[3] = (uint8_t)((nr >> 16) & 0xFF);
            stub[4] = (uint8_t)((nr >> 24) & 0xFF);
            stub[5] = 0xCD; /* int 0x80 */
            stub[6] = 0x80;
            stub[7] = 0xC3; /* ret */
            fail = guest_copy_in(pml4,
                                 WIN32_STUB_VA + (uint64_t)i * 16,
                                 stub, sizeof(stub)) != 0;
            uint64_t stub_va = WIN32_STUB_VA + (uint64_t)i * 16;
            if (!fail) {
                fail = guest_copy_in(
                           pml4, load + info.imports[i].iat_rva,
                           (const uint8_t *)&stub_va, 8) != 0;
            }
        }
    }
    if (!fail) {
        static const uint8_t exit_stub[] = {
            0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax, 0 (exit) */
            0x31, 0xFF,                   /* xor edi, edi */
            0xCD, 0x80,                   /* int 0x80 */
            0xF4                          /* hlt */
        };
        fail = guest_copy_in(pml4, GUEST_STUB_VA, exit_stub,
                             sizeof(exit_stub)) != 0;
    }
    if (!fail) {
        /* コマンドライン文字列 (名前 + 引数)。 */
        char cmd[1000];
        size_t cp = 0;
        const char *parts[17];
        parts[0] = name;
        int np = 1;
        for (int i = 0; i < nargs && np < 17; i++) {
            parts[np++] = args[i];
        }
        for (int i = 0; i < np; i++) {
            if (i > 0 && cp + 1 < sizeof(cmd)) {
                cmd[cp++] = ' ';
            }
            const char *s = parts[i];
            while (*s != '\0' && cp + 1 < sizeof(cmd)) {
                cmd[cp++] = *s++;
            }
        }
        cmd[cp] = '\0';
        fail = guest_copy_in(pml4, cmdline_va,
                             (const uint8_t *)cmd, cp + 1) != 0;
    }
    /* スタック。 */
    uint64_t stack_phys = 0;
    if (!fail) {
        stack_phys = pmm_alloc_pages(COMPAT_STACK_SIZE / 4096);
        if (stack_phys == 0) {
            fail = 1;
        }
    }
    for (uint64_t p = 0; p < COMPAT_STACK_SIZE && !fail;
         p += 4096) {
        fail = sandbox_map(pml4, GUEST_STACK_VA + p,
                           stack_phys + p, 1, 0) != 0;
    }
    if (fail) {
        sandbox_destroy(pml4);
        serial_write_line("run: out of memory");
        return -3;
    }
    uint8_t *stack_alias =
        (uint8_t *)(uintptr_t)(stack_phys + g_hhdm_offset);
    /* 戻り先 = 終了スタブ (入口が ret したら exit する)。 */
    uint64_t stub_va = GUEST_STUB_VA;
    memcpy(stack_alias + COMPAT_STACK_SIZE - 8, &stub_va, 8);
    uint64_t sp_guest = GUEST_STACK_VA + COMPAT_STACK_SIZE - 8;
    compat_set_context(&g_kernel_io, &img, kmalloc, kfree);
    compat_set_ranges(&img, (const uint8_t *)(uintptr_t)GUEST_STACK_VA,
                      COMPAT_STACK_SIZE);
    compat_reset_state();
    compat_ext_reset();
    pe_io pio;
    pio.io = &g_kernel_io;
    pio.sleep_ms = pe_sleep_ms;
    pio.tick_ms = pe_tick_ms;
    pio.cmdline_va = cmdline_va;
    pio.image_name = name;
    pio.unix_time = pe_unix_time;
    pe_set_context(&info, &pio, &img);
    compat_guest_cr3 = pml4;
    compat_guest_active = 1;
    sandbox_watchdog_start();
    int code = compat_call_entry(img.entry, sp_guest);
    sandbox_watchdog_stop();
    compat_guest_active = 0;
    compat_guest_cr3 = 0;
    pe_clear_context();
    compat_clear_ranges();
    compat_set_context(NULL, NULL, NULL, NULL);
    sandbox_destroy(pml4);
    if (!compat_exit_pending) {
        if (code == COMPAT_KILL_WATCHDOG) {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(
                ": guest killed (time limit, sandbox)");
        } else {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(": guest killed (fault, sandbox)");
        }
        if (exit_code != NULL) {
            *exit_code = code;
        }
        return -4;
    }
    if (exit_code != NULL) {
        *exit_code = compat_exit_code;
    }
    return 0;
}

/* ---- Mach-O ゲスト ----
 *
 * ELF と同じサンドボックスに Mach-O を載せる。セグメントを
 * その vmaddr に張り、LC_MAIN (無ければ LC_UNIXTHREAD) の
 * 入口から走らせる。ゲストは PrismOS の互換呼び出し
 * (int $0x80) を直接使う。dyld を要するものは macho_parse
 * の時点で断ってある。 */
static int run_macho(const char *name, const uint8_t *data,
                     size_t len, const char *const *args,
                     int nargs, int *exit_code) {
    (void)args;
    (void)nargs;
    macho_info info;
    int rc = macho_parse(data, len, &info);
    if (rc != 0) {
        serial_write("run: ");
        serial_write(name);
        serial_write(": ");
        serial_write_line(macho_error(rc));
        return -2;
    }
    /* セグメントはユーザー空間の低い方に収まること。 */
    uint64_t top = 0;
    for (int i = 0; i < info.seg_count; i++) {
        const macho_segment *s = &info.segs[i];
        if (s->vmaddr < 0x10000 ||
            s->vmaddr + s->vmsize > GUEST_STUB_VA ||
            s->vmaddr + s->vmsize < s->vmaddr) {
            serial_write_line(
                "run: Mach-O segment outside user space");
            return -2;
        }
        if (s->vmaddr + s->vmsize > top) {
            top = s->vmaddr + s->vmsize;
        }
    }
    uint64_t pml4 = sandbox_create();
    if (pml4 == 0) {
        return -3;
    }
    compat_image img;
    img_init(&img);
    img.base = info.image_base;
    img.entry = info.entry;
    int fail = 0;
    for (int i = 0; i < info.seg_count && !fail; i++) {
        const macho_segment *s = &info.segs[i];
        uint64_t first = s->vmaddr & ~0xFFFULL;
        uint64_t last =
            (s->vmaddr + s->vmsize + 0xFFFULL) & ~0xFFFULL;
        int writable = (s->initprot & 2) != 0;
        int executable = (s->initprot & 4) != 0;
        for (uint64_t p = first; p < last && !fail; p += 4096) {
            fail = sandbox_map_new(pml4, p, writable, executable,
                                   NULL) != 0;
        }
        if (!fail && s->filesize > 0) {
            fail = guest_copy_in(pml4, s->vmaddr,
                                 data + s->fileoff,
                                 (size_t)s->filesize) != 0;
        }
        if (!fail && img.seg_count < COMPAT_MAX_SEGMENTS) {
            int slot = img.seg_count++;
            img.seg_base[slot] = (uint8_t *)(uintptr_t)s->vmaddr;
            img.seg_vaddr[slot] = s->vmaddr;
            img.seg_memsz[slot] = last - first;
        }
    }
    /* brk 領域 (ヒープ用)。 */
    uint64_t brk_va = (top + 0xFFFULL) & ~0xFFFULL;
    if (!fail && brk_va + COMPAT_BRK_SIZE > GUEST_STUB_VA) {
        serial_write_line("run: image collides with the stub area");
        fail = 1;
    }
    for (uint64_t p = 0; p < COMPAT_BRK_SIZE && !fail; p += 4096) {
        fail = sandbox_map_new(pml4, brk_va + p, 1, 0, NULL) != 0;
    }
    if (!fail) {
        img.brk_base = (uint8_t *)(uintptr_t)brk_va;
        img.brk_cur = brk_va;
    }
    /* 終了スタブ (入口が ret したら exit する)。 */
    if (!fail) {
        static const uint8_t exit_stub[] = {
            0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax, 0 (exit) */
            0x31, 0xFF,                   /* xor edi, edi */
            0xCD, 0x80,                   /* int 0x80 */
            0xF4                          /* hlt */
        };
        fail = sandbox_map_new(pml4, GUEST_STUB_VA, 0, 1,
                               NULL) != 0 ||
               guest_copy_in(pml4, GUEST_STUB_VA, exit_stub,
                             sizeof(exit_stub)) != 0;
    }
    /* スタック。 */
    uint64_t stack_phys = 0;
    if (!fail) {
        stack_phys = pmm_alloc_pages(COMPAT_STACK_SIZE / 4096);
        if (stack_phys == 0) {
            fail = 1;
        }
    }
    for (uint64_t p = 0; p < COMPAT_STACK_SIZE && !fail;
         p += 4096) {
        fail = sandbox_map(pml4, GUEST_STACK_VA + p,
                           stack_phys + p, 1, 0) != 0;
    }
    if (fail) {
        sandbox_destroy(pml4);
        serial_write_line("run: out of memory");
        return -3;
    }
    uint8_t *stack_alias =
        (uint8_t *)(uintptr_t)(stack_phys + g_hhdm_offset);
    /* 戻り先 = 終了スタブ (入口が ret したら exit する)。 */
    uint64_t stub_va = GUEST_STUB_VA;
    memcpy(stack_alias + COMPAT_STACK_SIZE - 8, &stub_va, 8);
    uint64_t sp_guest = GUEST_STACK_VA + COMPAT_STACK_SIZE - 8;
    compat_set_context(&g_kernel_io, &img, kmalloc, kfree);
    compat_set_ranges(&img, (const uint8_t *)(uintptr_t)GUEST_STACK_VA,
                      COMPAT_STACK_SIZE);
    compat_reset_state();
    compat_ext_reset();
    compat_guest_cr3 = pml4;
    compat_guest_active = 1;
    sandbox_watchdog_start();
    int code = compat_call_entry(img.entry, sp_guest);
    sandbox_watchdog_stop();
    compat_guest_active = 0;
    compat_guest_cr3 = 0;
    compat_clear_ranges();
    compat_set_context(NULL, NULL, NULL, NULL);
    sandbox_destroy(pml4);
    if (!compat_exit_pending) {
        if (code == COMPAT_KILL_WATCHDOG) {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(
                ": guest killed (time limit, sandbox)");
        } else {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(": guest killed (fault, sandbox)");
        }
        if (exit_code != NULL) {
            *exit_code = code;
        }
        return -4;
    }
    if (exit_code != NULL) {
        *exit_code = compat_exit_code;
    }
    return 0;
}

int compat_run_file(const char *name, const char *const *args, int nargs,
                    int *exit_code) {
    size_t len = 0;
    const char *data = vfs_read_ptr(name, &len);
    if (data == NULL) {
        /* .app バンドル: 中の実行ファイルを探す。 */
        size_t nlen = strlen(name);
        if (nlen > 4 && strcmp(name + nlen - 4, ".app") == 0) {
            char inner[128];
            size_t base_len = nlen - 4;
            if (nlen + 32 < sizeof(inner)) {
                size_t p = 0;
                for (size_t i = 0; i < nlen; i++) {
                    inner[p++] = name[i];
                }
                const char *tail = "/Contents/MacOS/";
                for (size_t i = 0; tail[i] != '\0'; i++) {
                    inner[p++] = tail[i];
                }
                for (size_t i = 0; i < base_len; i++) {
                    inner[p++] = name[i];
                }
                inner[p] = '\0';
                data = vfs_read_ptr(inner, &len);
                if (data != NULL) {
                    return compat_run_file(inner, args, nargs,
                                           exit_code);
                }
            }
        }
        serial_write("run: ");
        serial_write(name);
        serial_write_line(": No such file");
        return -1;
    }
    if (len >= 2 && data[0] == 'M' && data[1] == 'Z') {
        return run_pe(name, (const uint8_t *)data, len, args,
                      nargs, exit_code);
    }
    if (len >= 4 && (uint8_t)data[0] == 0xCF &&
        (uint8_t)data[1] == 0xFA && (uint8_t)data[2] == 0xED &&
        (uint8_t)data[3] == 0xFE) {
        return run_macho(name, (const uint8_t *)data, len, args,
                         nargs, exit_code);
    }
    compat_elf_info info;
    int rc = compat_elf_parse((const uint8_t *)data, len, &info);
    if (rc != 0) {
        serial_write("run: ");
        serial_write(name);
        serial_write(": ");
        serial_write_line(compat_elf_error(rc));
        return -2;
    }
    compat_image img;
    uint64_t pml4 = 0;
    uint64_t stack_phys = 0;
    if (info.dyn.present) {
        if (sandbox_load_dynamic(&info, (const uint8_t *)data, name,
                                 &pml4, &stack_phys, &img) != 0) {
            return -3;
        }
    } else if (sandbox_load(&info, (const uint8_t *)data, &pml4,
                            &stack_phys, &img) != 0) {
        serial_write_line("run: out of memory");
        return -3;
    }
    uint8_t *stack_alias =
        (uint8_t *)(uintptr_t)(stack_phys + g_hhdm_offset);
    compat_set_context(&g_kernel_io, &img, kmalloc, kfree);
    compat_set_ranges(&img, (const uint8_t *)(uintptr_t)GUEST_STACK_VA,
                      COMPAT_STACK_SIZE);
    compat_reset_state();
    compat_ext_reset();
    if (info.dyn.present &&
        dyn_run_inits(pml4, stack_phys) != 0) {
        compat_guest_active = 0;
        compat_guest_cr3 = 0;
        compat_clear_ranges();
        compat_set_context(NULL, NULL, NULL, NULL);
        sandbox_destroy(pml4);
        return -3;
    }
    uint64_t sp_guest = build_stack(
        name, args, nargs, stack_alias, COMPAT_STACK_SIZE,
        GUEST_STACK_VA - (uint64_t)(uintptr_t)stack_alias);
    compat_guest_cr3 = pml4;
    compat_guest_active = 1;
    sandbox_watchdog_start();
    int code = compat_call_entry(img.entry, sp_guest);
    sandbox_watchdog_stop();
    compat_guest_active = 0;
    compat_guest_cr3 = 0;
    compat_clear_ranges();
    compat_set_context(NULL, NULL, NULL, NULL);
    /* スタックのページもゲストのテーブル経由で解放される。 */
    sandbox_destroy(pml4);
    if (!compat_exit_pending) {
        /* exit を呼ばずに戻ってきた = フォルトかウォッチ
         * ドッグで止められた。理由を表示する。 */
        if (code == COMPAT_KILL_WATCHDOG) {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(
                ": guest killed (time limit, sandbox)");
        } else {
            serial_write("run: ");
            serial_write(name);
            serial_write_line(": guest killed (fault, sandbox)");
        }
        if (exit_code != NULL) {
            *exit_code = code;
        }
        return -4;
    }
    if (exit_code != NULL) {
        *exit_code = code;
    }
    return 0;
}

int compat_info_file(const char *name, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0) {
        return -1;
    }
    out[0] = '\0';
    size_t len = 0;
    const char *data = vfs_read_ptr(name, &len);
    if (data == NULL) {
        return -1;
    }
    if (len >= 4 && (uint8_t)data[0] == 0xCF &&
        (uint8_t)data[1] == 0xFA && (uint8_t)data[2] == 0xED &&
        (uint8_t)data[3] == 0xFE) {
        macho_info minfo;
        if (macho_parse((const uint8_t *)data, len, &minfo) != 0) {
            return -2;
        }
        size_t pos = 0;
        const char *head = "Mach-O 64-bit x86-64, type EXECUTE, "
                           "entry 0x";
        while (*head != '\0' && pos + 1 < out_cap) {
            out[pos++] = *head++;
        }
        char hex[17];
        int n = 0;
        uint64_t v = minfo.entry;
        if (v == 0) {
            hex[n++] = '0';
        } else {
            char rev[16];
            int r = 0;
            while (v > 0) {
                rev[r++] = "0123456789abcdef"[v & 0xF];
                v >>= 4;
            }
            while (r > 0) {
                hex[n++] = rev[--r];
            }
        }
        hex[n] = '\0';
        for (int i = 0; hex[i] != '\0' && pos + 1 < out_cap; i++) {
            out[pos++] = hex[i];
        }
        const char *tail = ", segments: ";
        while (*tail != '\0' && pos + 1 < out_cap) {
            out[pos++] = *tail++;
        }
        char num[4];
        n = 0;
        int sv = minfo.seg_count;
        if (sv == 0) {
            num[n++] = '0';
        } else {
            char rev[4];
            int r = 0;
            while (sv > 0) {
                rev[r++] = (char)('0' + (sv % 10));
                sv /= 10;
            }
            while (r > 0) {
                num[n++] = rev[--r];
            }
        }
        num[n] = '\0';
        for (int i = 0; num[i] != '\0' && pos + 1 < out_cap; i++) {
            out[pos++] = num[i];
        }
        out[pos] = '\0';
        return 0;
    }
    compat_elf_info info;
    int rc = compat_elf_parse((const uint8_t *)data, len, &info);
    if (rc != 0) {
        return -2;
    }
    size_t pos = 0;
    /* 小さな追記ヘルパー (snprintf は無いため自前で)。 */
    const char *parts[] = {
        "ELF 64-bit x86-64, type ",
        info.is_exec ? "EXEC" : "DYN",
        ", entry 0x",
        NULL,
    };
    for (int i = 0; parts[i] != NULL; i++) {
        const char *s = parts[i];
        while (*s != '\0' && pos + 1 < out_cap) {
            out[pos++] = *s++;
        }
    }
    /* 入口を 16 進で書く。 */
    {
        char hex[17];
        int n = 0;
        uint64_t v = info.entry;
        if (v == 0) {
            hex[n++] = '0';
        } else {
            char rev[16];
            int r = 0;
            while (v > 0) {
                rev[r++] = "0123456789abcdef"[v & 0xF];
                v >>= 4;
            }
            while (r > 0) {
                hex[n++] = rev[--r];
            }
        }
        hex[n] = '\0';
        for (int i = 0; hex[i] != '\0' && pos + 1 < out_cap; i++) {
            out[pos++] = hex[i];
        }
    }
    {
        const char *tail = ", segments: ";
        while (*tail != '\0' && pos + 1 < out_cap) {
            out[pos++] = *tail++;
        }
    }
    {
        char num[4];
        int n = 0;
        int v = info.seg_count;
        if (v == 0) {
            num[n++] = '0';
        } else {
            char rev[4];
            int r = 0;
            while (v > 0) {
                rev[r++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (r > 0) {
                num[n++] = rev[--r];
            }
        }
        num[n] = '\0';
        for (int i = 0; num[i] != '\0' && pos + 1 < out_cap; i++) {
            out[pos++] = num[i];
        }
    }
    for (int i = 0; i < info.seg_count; i++) {
        const char *sep = (i == 0) ? " [" : ", ";
        while (*sep != '\0' && pos + 1 < out_cap) {
            out[pos++] = *sep++;
        }
        /* "vaddr+memsz bytes" を 10 進で書く。 */
        char num[21];
        int n = 0;
        uint64_t v = info.segs[i].vaddr;
        if (v == 0) {
            num[n++] = '0';
        } else {
            char rev[21];
            int r = 0;
            while (v > 0) {
                rev[r++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (r > 0) {
                num[n++] = rev[--r];
            }
        }
        num[n] = '\0';
        for (int j = 0; num[j] != '\0' && pos + 1 < out_cap; j++) {
            out[pos++] = num[j];
        }
        const char *plus = "+";
        while (*plus != '\0' && pos + 1 < out_cap) {
            out[pos++] = *plus++;
        }
        n = 0;
        v = info.segs[i].memsz;
        if (v == 0) {
            num[n++] = '0';
        } else {
            char rev[21];
            int r = 0;
            while (v > 0) {
                rev[r++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (r > 0) {
                num[n++] = rev[--r];
            }
        }
        num[n] = '\0';
        for (int j = 0; num[j] != '\0' && pos + 1 < out_cap; j++) {
            out[pos++] = num[j];
        }
    }
    if (info.seg_count > 0 && pos + 1 < out_cap) {
        out[pos++] = ']';
    }
    out[pos] = '\0';
    return 0;
}

int compat_is_app(const char *name) {
    if (name == NULL) {
        return 0;
    }
    size_t n = strlen(name);
    if (n < 4) {
        return 0;
    }
    if (strcmp(name + n - 4, ".elf") == 0 ||
        strcmp(name + n - 4, ".exe") == 0 ||
        strcmp(name + n - 4, ".app") == 0) {
        return 1;
    }
    if (n >= 6 && strcmp(name + n - 6, ".macho") == 0) {
        return 1;
    }
    return 0;
}
