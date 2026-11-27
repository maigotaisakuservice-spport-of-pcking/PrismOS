/* PrismOS — 32bit カーネルメイン (Multiboot1、x86 32bit)
 *
 * 起動の流れ:
 *   1. COM1 シリアルを初期化し、バナーを出す
 *   2. Multiboot 情報からメモリ量を読む
 *   3. ヒープ (静的アリーナ) と VFS を初期化し、x86-64 版・
 *      ARM 版と同じシェルコア (shell/core.c) を動かす
 *   4. シリアルから 1 行ずつ読み、シェルコアに渡す
 *
 * 64bit 版との違い: long mode に入らない、ページングなし。
 * ディスク (ATA + diskfs)、ネットワーク (e1000 + 共有
 * netstack)、GUI デスクトップ (Bochs VBE フレームバッファ、
 * `desktop` コマンド) を備える。シェルとファイルシステムは
 * 64bit 版と同一のソースが動く。32bit 専用 CPU でも起動
 * できることが要点で、v86 のような 32bit エミュレータでも
 * 動く形にしてある。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "ata32.h"
#include "diskfs.h"
#include "kernel32.h"
#include "netstack.h"
#include "string.h"
#include "vfs.h"

#define COM1 0x3F8

static void m32_write(const char *s);

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void m32_serial_init(void) {
    outb(COM1 + 1, 0x00); /* 割り込み無効 */
    outb(COM1 + 3, 0x80); /* DLAB: 分周比を設定する */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1、DLAB 解除 */
    outb(COM1 + 2, 0xC7); /* FIFO 有効、送受信クリア、14 バイト閾値 */
    outb(COM1 + 4, 0x0B); /* DTR + RTS + OUT2 */
}

/* ---- VGA テキストコンソール (0xB8000、80x25) ----
 * シリアル出力のミラー。32bit 版でも画面で読めるようにする。 */
#define VGA_TEXT ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x07

static int vga_x;
static int vga_y;

static void vga_scroll(void) {
    int x, y;
    for (y = 1; y < VGA_ROWS; y++) {
        for (x = 0; x < VGA_COLS; x++) {
            VGA_TEXT[(y - 1) * VGA_COLS + x] = VGA_TEXT[y * VGA_COLS + x];
        }
    }
    for (x = 0; x < VGA_COLS; x++) {
        VGA_TEXT[(VGA_ROWS - 1) * VGA_COLS + x] =
            (uint16_t)((VGA_ATTR << 8) | ' ');
    }
}

/* VGA をテキストモード (モード 3) に設定する。ブートローダは
 * グラフィックスのフレームバッファで渡してくるので、そのまま
 * では 0xB8000 への書き込みが画面に出ない。レジスタ値は
 * VGA の標準モード 3 のもの (公開仕様)。 */
static void vga_set_text_mode(void) {
    static const uint8_t seq[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
    static const uint8_t crtc[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x4F, 0x96, 0xB9, 0xA3,
        0xFF,
    };
    static const uint8_t gfx[9] = {0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x10, 0x0E, 0x00, 0xFF};
    static const uint8_t attr[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x01, 0x00, 0x0F, 0x00, 0x00,
    };
    int i;
    /* Bochs VBE が有効なままだとテキストモードに切り替わら
     * ないので、先に VBE を無効化する (0x01CE = インデックス、
     * 0x01CF = データ、インデックス 4 = 有効フラグ)。 */
    outw(0x01CE, 0x0004);
    outw(0x01CF, 0x0000);
    outb(0x3C2, 0x67); /* その他出力レジスタ */
    for (i = 0; i < 5; i++) {
        outb(0x3C4, (uint8_t)i);
        outb(0x3C5, seq[i]);
    }
    outb(0x3D4, 0x11);
    outb(0x3D5, 0x00); /* CRTC のロック解除 */
    for (i = 0; i < 25; i++) {
        outb(0x3D4, (uint8_t)i);
        outb(0x3D5, crtc[i]);
    }
    for (i = 0; i < 9; i++) {
        outb(0x3CE, (uint8_t)i);
        outb(0x3CF, gfx[i]);
    }
    (void)inb(0x3DA); /* 属性コントローラのフリップフロップ初期化 */
    for (i = 0; i < 21; i++) {
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, attr[i]);
    }
    outb(0x3C0, 0x20); /* 画面表示を有効化 */
}

static void vga_init(void) {
    int i;
    vga_set_text_mode();
    for (i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_TEXT[i] = (uint16_t)((VGA_ATTR << 8) | ' ');
    }
    vga_x = 0;
    vga_y = 0;
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_x = 0;
        vga_y++;
    } else if (c == '\r') {
        vga_x = 0;
    } else if (c == '\b') {
        if (vga_x > 0) {
            vga_x--;
        }
    } else {
        VGA_TEXT[vga_y * VGA_COLS + vga_x] =
            (uint16_t)((VGA_ATTR << 8) | (uint8_t)c);
        vga_x++;
        if (vga_x >= VGA_COLS) {
            vga_x = 0;
            vga_y++;
        }
    }
    if (vga_y >= VGA_ROWS) {
        vga_scroll();
        vga_y = VGA_ROWS - 1;
    }
}

/* GUI デスクトップが端末描画のために受ける口 (任意)。 */
static void (*con_sink)(char c);
static void (*mouse_sink)(uint8_t byte);

void main32_set_con_sink(void (*sink)(char c)) {
    con_sink = sink;
}

void main32_set_mouse_sink(void (*sink)(uint8_t byte)) {
    mouse_sink = sink;
}

static void m32_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);
    vga_putc(c);
    if (con_sink != NULL) {
        con_sink(c);
    }
}

void main32_con_putc(char c) {
    m32_putc(c);
}

void main32_con_write(const char *s) {
    m32_write(s);
}

/* VGA テキストモードに戻す (GUI 終了時)。 */
void main32_text_mode(void) {
    vga_init();
}

static void m32_write(const char *s) {
    while (*s) {
        if (*s == '\n') {
            m32_putc('\r');
        }
        m32_putc(*s);
        s++;
    }
}

static void m32_write_u32(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (v == 0) {
        m32_putc('0');
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        buf[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    m32_write(&buf[i]);
}

static void m32_write_hex32(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    int i;
    m32_write("0x");
    for (i = 28; i >= 0; i -= 4) {
        m32_putc(digits[(v >> i) & 0xfu]);
    }
}

/* ---- PS/2 キーボード (ポーリング、スキャンコード集合 1) ----
 * 画面で使う人のために、シリアルと並んでキーボードからも
 * 読む。対応は基本的な US 配列 (英数と記号、Shift)。 */
static const char kbd_map[0x3A] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', '\n', 0,  'a', 's', 'd', 'f', 'g', 'h',
    'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,  '*', 0,  ' ',
};
static const char kbd_map_shift[0x3A] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n', 0,  'A', 'S', 'D', 'F', 'G', 'H',
    'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,  '*', 0,  ' ',
};

static int kbd_shift;

/* キーボードに来ていれば 1 文字返す。なければ 0。 */
static int kbd_try_read(void) {
    if ((inb(0x64) & 0x01) == 0) {
        return 0;
    }
    if ((inb(0x64) & 0x20) != 0) {
        uint8_t mb = inb(0x60);
        if (mouse_sink != NULL) {
            mouse_sink(mb); /* GUI がマウスとして使う */
        }
        return 0;
    }
    uint8_t sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) {
        kbd_shift = 1;
        return 0;
    }
    if (sc == 0xAA || sc == 0xB6) {
        kbd_shift = 0;
        return 0;
    }
    if ((sc & 0x80) != 0 || sc >= 0x3A) {
        return 0;
    }
    char c = kbd_shift ? kbd_map_shift[sc] : kbd_map[sc];
    return (uint8_t)c;
}

/* 1 行読む。エコーつき。Backspace を処理する。
 * 入力元はシリアルと PS/2 キーボードの両方。 */
static size_t m32_read_line(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c;
        for (;;) {
            if ((inb(COM1 + 5) & 0x01) != 0) {
                c = (char)inb(COM1);
                break;
            }
            int k = kbd_try_read();
            if (k != 0) {
                c = (char)k;
                break;
            }
        }
        if (c == '\r' || c == '\n') {
            m32_write("\r\n");
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                m32_write("\b \b");
            }
            continue;
        }
        if (n + 1 < cap) {
            buf[n++] = c;
            m32_putc(c);
        }
    }
    buf[n] = '\0';
    return n;
}

/* 入力を 1 文字だけ覗く (ブロッキングしない)。
 * 0 = まだ無い。GUI のイベントループ用。 */
int main32_poll_char(void) {
    if ((inb(COM1 + 5) & 0x01) != 0) {
        return inb(COM1);
    }
    return kbd_try_read();
}

/* 8042 キーボードコントローラ経由のリセット。戻ってきたら
 * 空の IDT で割り込みを起こしてトリプルフォルトさせる。 */
static void reboot_now(void) {
    static const struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) empty_idt = {0, 0};
    int i;
    for (i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            break;
        }
    }
    outb(0x64, 0xFE);
    __asm__ volatile("lidt %0" : : "m"(empty_idt));
    __asm__ volatile("int3");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* Multiboot 情報 (先頭部分だけ使う) */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower; /* 640KB までの KB 数 */
    uint32_t mem_upper; /* 1MB より上の KB 数 */
} multiboot_info_t;

static uint32_t total_kb;

static uint32_t disk32_sectors;

/* ---- 永続ファイルシステム (diskfs on ATA) ----
 * 64bit 版・ARM 版と同じ diskfs 形式を使う。起動時に
 * フォーマット済みなら全ファイルを VFS へ読み込み、
 * dsync で書き戻す。32bit 版にはアカウント/ログインが
 * 無いため暗号化はしない (平文)。 */
static diskfs_dev fs32_dev;
static int fs32_ready;
static uint8_t fs32_buf[DISKFS_MAX_FILE_SIZE];
static diskfs_entry fs32_entries[DISKFS_MAX_FILES];

static int fs32_read_sector(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    return ata32_read(lba, buf);
}

static int fs32_write_sector(void *ctx, uint32_t lba,
                             const uint8_t *buf) {
    (void)ctx;
    return ata32_write(lba, buf);
}

static int fs32_load(void) {
    int count = 0;
    size_t len = 0;
    if (!fs32_ready ||
        diskfs_list(&fs32_dev, fs32_entries, &count) != 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!fs32_entries[i].used) {
            continue;
        }
        if (diskfs_read_file(&fs32_dev, fs32_entries[i].name,
                             fs32_buf, sizeof(fs32_buf), &len) == 0) {
            vfs_write(fs32_entries[i].name, (const char *)fs32_buf,
                      len);
        }
    }
    return count;
}

static int fs32_sync(void) {
    int n = vfs_file_count(), written = 0;
    if (!fs32_ready) {
        return -1;
    }
    if (diskfs_is_formatted(&fs32_dev) != 1) {
        if (diskfs_format(&fs32_dev) != 0) {
            return -1;
        }
        m32_write("[disk] formatted the disk\n");
    }
    for (int i = 0; i < n; i++) {
        const char *name = vfs_file_name(i);
        size_t len = 0;
        const char *data = vfs_read_ptr(name, &len);
        if (data == NULL) {
            continue;
        }
        if (diskfs_write_file(&fs32_dev, name,
                              (const uint8_t *)data, len) == 0) {
            written++;
        }
    }
    return written;
}

static void cmd_dsync(void) {
    int w = fs32_sync();
    if (w < 0) {
        m32_write("dsync: failed (no disk?)\n");
        return;
    }
    m32_write("dsync: ");
    m32_write_u32((uint32_t)w);
    m32_write(" file(s) written to disk\n");
}

static void cmd_dload(void) {
    int c = fs32_load();
    if (c < 0) {
        m32_write("dload: failed (not formatted?)\n");
        return;
    }
    m32_write("dload: ");
    m32_write_u32((uint32_t)c);
    m32_write(" file(s) loaded from disk\n");
}

static void cmd_dls(void) {
    int count = 0;
    if (!fs32_ready ||
        diskfs_list(&fs32_dev, fs32_entries, &count) != 0) {
        m32_write("dls: failed (not formatted?)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (fs32_entries[i].used) {
            m32_write(fs32_entries[i].name);
            m32_write("  ");
            m32_write_u32(fs32_entries[i].size);
            m32_write(" bytes\n");
        }
    }
}

static void cmd_disk(void) {
    if (disk32_sectors == 0) {
        m32_write("disk: (no ATA drive)\n");
        return;
    }
    m32_write("disk: ata ");
    m32_write_u32(disk32_sectors);
    m32_write(" sectors\n");
}

static void cmd_disk_test(void) {
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    if (disk32_sectors == 0) {
        m32_write("disk test: no drive\n");
        return;
    }
    uint32_t lba = disk32_sectors - 1;
    for (int i = 0; i < 512; i++) {
        wbuf[i] = (uint8_t)(i * 31 + 7);
    }
    if (ata32_write(lba, wbuf) != 0) {
        m32_write("disk test: write failed\n");
        return;
    }
    for (int i = 0; i < 512; i++) {
        rbuf[i] = 0;
    }
    if (ata32_read(lba, rbuf) != 0) {
        m32_write("disk test: read failed\n");
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) {
            m32_write("disk test: mismatch\n");
            return;
        }
    }
    m32_write("disk test: ok\n");
}

static void cmd_mem(void) {
    m32_write("memory: ");
    m32_write_u32(total_kb);
    m32_write(" KB total (");
    m32_write_u32(total_kb / 1024);
    m32_write(" MB), heap ");
    m32_write_u32((uint32_t)alloc32_free_bytes());
    m32_write(" bytes free\n");
}

/* ---- ネットワーク (共有 netstack + e1000) ---- */

static void write_ip(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            m32_write(".");
        }
        m32_write_u32(ip[i]);
    }
}

static void cmd_dhcp(void) {
    if (net_ensure() != 0) {
        m32_write("dhcp: failed (no network card?)\n");
        return;
    }
    uint8_t ip[4];
    net_my_ip(ip);
    m32_write("dhcp: address ");
    write_ip(ip);
    m32_write("\n");
}

static void cmd_netinfo(void) {
    if (!net_ready()) {
        m32_write("net: not configured (run dhcp)\n");
        return;
    }
    uint8_t ip[4];
    m32_write("net: ip ");
    net_my_ip(ip);
    write_ip(ip);
    m32_write(" router ");
    net_my_router(ip);
    write_ip(ip);
    m32_write(" dns ");
    net_my_dns(ip);
    write_ip(ip);
    m32_write("\n");
}

static void cmd_ping(const char *arg) {
    uint8_t ip[4];
    if (net_parse_ipv4(arg, ip) != 0) {
        m32_write("ping: bad address\n");
        return;
    }
    if (net_ensure() != 0) {
        m32_write("ping: network unavailable\n");
        return;
    }
    if (net_ping(ip, 1, 3) == 0) {
        m32_write("ping: reply from ");
        write_ip(ip);
        m32_write("\n");
    } else {
        m32_write("ping: no reply\n");
    }
}

/* 1 行を実行する: ローカルコマンド → シェルコアの順。
 * テキストモードのループと GUI の端末窓で共用する。 */
void main32_exec_line(prism_shell_state *state, const char *line) {
    static char out[4096];
    if (strcmp(line, "mem") == 0) {
        cmd_mem();
        return;
    }
    if (strcmp(line, "disk") == 0) {
        cmd_disk();
        return;
    }
    if (strcmp(line, "disk test") == 0) {
        cmd_disk_test();
        return;
    }
    if (strcmp(line, "dsync") == 0) {
        cmd_dsync();
        return;
    }
    if (strcmp(line, "dload") == 0) {
        cmd_dload();
        return;
    }
    if (strcmp(line, "dls") == 0) {
        cmd_dls();
        return;
    }
    if (strcmp(line, "dhcp") == 0) {
        cmd_dhcp();
        return;
    }
    if (strcmp(line, "netinfo") == 0) {
        cmd_netinfo();
        return;
    }
    if (strncmp(line, "ping ", 5) == 0) {
        cmd_ping(line + 5);
        return;
    }
    if (strcmp(line, "desktop") == 0) {
        if (desktop32_available()) {
            desktop32_run(state);
        } else {
            m32_write(
                "desktop: no Bochs VBE graphics adapter\n");
        }
        return;
    }
    prism_shell_execute(state, line, out, sizeof(out));
    m32_write(out);
    if (state->should_reboot) {
        reboot_now();
    }
    if (state->should_clear) {
        state->should_clear = 0;
        m32_write("--- cleared ---\n");
    }
}

void kmain32(uint32_t magic, uint32_t info_ptr) {
    prism_shell_state state;
    static char line[PRISM_SHELL_LINE_CAP];
    const multiboot_info_t *info = (const multiboot_info_t *)(uintptr_t)info_ptr;

    m32_serial_init();
    vga_init();
    m32_write("PrismOS 27H1 (32-bit)\n");
    m32_write("A customizable OS for everyone.\n");

    if (magic == 0x2BADB002u && (info->flags & 0x1u)) {
        total_kb = info->mem_lower + info->mem_upper;
        m32_write("[boot] multiboot: memory ");
        m32_write_u32(total_kb / 1024);
        m32_write(" MB\n");
    } else {
        total_kb = 0;
        m32_write("[boot] multiboot: no memory info (magic=");
        m32_write_hex32(magic);
        m32_write(" flags=");
        m32_write_hex32(info->flags);
        m32_write(")\n");
    }

    alloc32_init();
    vfs_set_allocator(alloc32, free32);
    net_set_hhdm(0); /* 恒等アドレス: オフセットなし */
    if (ata32_init(&disk32_sectors)) {
        m32_write("[boot] disk: ata ");
        m32_write_u32(disk32_sectors);
        m32_write(" sectors\n");
        fs32_dev.ctx = NULL;
        fs32_dev.read_sector = fs32_read_sector;
        fs32_dev.write_sector = fs32_write_sector;
        fs32_dev.total_sectors = disk32_sectors;
        fs32_dev.base_lba = 0;
        fs32_dev.crypto = NULL;
        fs32_ready = 1;
    }
    prism_shell_init(&state);
    if (fs32_ready && diskfs_is_formatted(&fs32_dev) == 1) {
        int loaded = fs32_load();
        m32_write("[boot] disk: ");
        m32_write_u32(loaded > 0 ? (uint32_t)loaded : 0);
        m32_write(" file(s) loaded from disk\n");
    } else if (fs32_ready) {
        m32_write("[boot] disk: not formatted (dsync formats it)\n");
    }

    m32_write("Type `help` to see available commands.\n");

    for (;;) {
        size_t n;
        m32_write("prismos> ");
        n = m32_read_line(line, sizeof(line));
        if (n == 0) {
            continue;
        }
        main32_exec_line(&state, line);
    }
}
