/* PrismOS — 仮想ファイルシステム: RAM 上のファイル置き場。
 * ファイルの中身はアロケータ (カーネルではヒープ) から確保する。 */
#include <stdint.h>

#include "vfs.h"

/* 起動時の組み込みサンプル: 写真アプリ用の小さな BMP と音楽
 * アプリ用の短い WAV、それに動画アプリ用の 24 フレームの
 * PV1 (圧縮動画、320x240、音声つき)。どれも本物のファイルで、1 ファイル
 * 16384 バイトの上限に収まる大きさ。元データは assets/ と
 * scripts/encode-video.py にあり、ここは生成したバイト列。
 * 互換レイヤーの実証ゲスト (hello.elf など 4 本) もここに
 * 入る。元は compat ディレクトリの .s ファイルで、
 * scripts/build-compat-guests.py が shell/sample_compat.h を作る。 */
#include "sample_bmp.h"
#include "sample_compat.h"
#include "sample_pe.h"
#include "sample_driver.h"
#include "sample_wav.h"
#include "sample_video_pv1.h"

/* inode 表に相当する部分: 名前・中身へのポインタ・大きさだけを
 * 持つ。中身そのものはここには置かない。 */
typedef struct {
    char name[PRISM_VFS_MAX_NAME + 1];
    char *data; /* アロケータから確保した領域 (空ファイルは NULL) */
    size_t size;
    int used;
    uint32_t mtime; /* 更新時刻 (Unix 秒、0 = 未記録) */
} vfs_file;

static vfs_file files[PRISM_VFS_MAX_FILES];
static vfs_file *find_slot(const char *name);
static uint32_t (*vfs_time_fn)(void);

void vfs_set_time_source(uint32_t (*fn)(void)) {
    vfs_time_fn = fn;
}

uint32_t vfs_file_mtime(int index) {
    if (index < 0 || index >= PRISM_VFS_MAX_FILES ||
        !files[index].used) {
        return 0;
    }
    return files[index].mtime;
}

void vfs_set_mtime(const char *name, uint32_t t) {
    vfs_file *f = find_slot(name);
    if (f != NULL) {
        f->mtime = t;
    }
}

static vfs_alloc_fn vfs_alloc;
static vfs_free_fn vfs_free;
static unsigned long vfs_seq;

unsigned long vfs_change_seq(void) {
    return vfs_seq;
}

static size_t vfs_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int vfs_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void vfs_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int vfs_name_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (vfs_len(name) > PRISM_VFS_MAX_NAME) {
        return 0;
    }
    /* '/' はディレクトリ区切りとして許可する。ただし
     * 先頭・末尾・連続は不可、各要素が "." や ".." も不可。 */
    size_t comp = 0;
    for (size_t i = 0; ; i++) {
        char c = name[i];
        if (c == '/' || c == '\0') {
            if (comp == 0) {
                return 0;
            }
            if (comp == 1 && name[i - 1] == '.') {
                return 0;
            }
            if (comp == 2 && name[i - 2] == '.' &&
                name[i - 1] == '.') {
                return 0;
            }
            comp = 0;
            if (c == '\0') {
                break;
            }
            continue;
        }
        if (c == '\\' || (unsigned char)c < 0x20) {
            return 0;
        }
        comp++;
    }
    return 1;
}

static vfs_file *find_slot(const char *name) {
    for (int i = 0; i < PRISM_VFS_MAX_FILES; i++) {
        if (files[i].used && vfs_eq(files[i].name, name)) {
            return &files[i];
        }
    }
    return NULL;
}

static void seed(const char *name, const char *content) {
    (void)vfs_write(name, content, vfs_len(content));
}

/* バイナリの組み込みファイル用 (NUL を含むことがあるため)。 */
static void seed_bytes(const char *name, const void *data, size_t len) {
    (void)vfs_write(name, (const char *)data, len);
}

/* 組み込みファイルの正しい中身 (vfs_init と修復で共用する)。
 * ここにあるものだけが「正しい組み込みファイル」で、ユーザーの
 * ファイルはここには無い。 */
static const struct {
    const char *name;
    const char *text;
} builtin_texts[] = {
    { "README.txt",
      "PrismOS 27H1\n"
      "A customizable OS: modern centered UI, Dock and quick search,\n"
      "and a shell with two command styles.\n" },
    { "VERSION", "27H1\n" },
    { "theme.toml",
      "[theme]\n"
      "accent = \"#4CC2FF\"\n"
      "wallpaper = \"prism-dusk.png\"\n"
      "taskbar_position = \"bottom\"\n" },
    { "privacy.txt",
      "PrismOS Privacy Policy\n"
      "\n"
      "PrismOS keeps your data on this device.\n"
      "- Accounts are local only. There is no Prism\n"
      "  account and no cloud sign-in.\n"
      "- PrismOS sends no telemetry: no usage data,\n"
      "  no crash reports, no identifiers leave the\n"
      "  device on their own.\n"
      "- Network features (updates, mail, fetching a\n"
      "  page) connect only to servers you configure,\n"
      "  and only when you use them.\n"
      "- Files at rest are protected with AES-256\n"
      "  encryption tied to your PIN, as described in\n"
      "  the security settings.\n"
      "- Audit logs stay on the device, encrypted.\n"
      "\n"
      "If this policy changes, the change ships with\n"
      "a feature update you choose to apply.\n" },
    { "terms.txt",
      "PrismOS Terms of Use\n"
      "\n"
      "- PrismOS is provided as-is, without warranty\n"
      "  of any kind, to the extent the law allows.\n"
      "- You may install PrismOS on your own devices\n"
      "  and use it for any lawful purpose.\n"
      "- Third-party components keep their own\n"
      "  licenses; see licenses.txt.\n"
      "- Loadable drivers must carry a valid ML-DSA\n"
      "  signature from a root key you control.\n"
      "  Installing a driver is your decision and\n"
      "  your responsibility.\n"
      "- Support is community Q&A on the project\n"
      "  README only.\n" },
    { "licenses.txt",
      "Third-party licenses\n"
      "\n"
      "Japanese font glyphs: Noto Sans CJK Regular,\n"
      "SIL Open Font License 1.1. Full text: OFL-1.1-Noto.txt\n"
      "in the licenses folder of the install image.\n"
      "\n"
      "Bootloader: Limine 8.3.2, BSD 2-Clause License.\n"
      "Full text: BSD-Limine.txt in the same folder.\n"
      "\n"
      "See NOTICE.txt on the install image for details.\n" },
    { "about.txt",
      "PrismOS is built to be customized anywhere:\n"
      "theme, wallpaper, taskbar and Dock from config/theme.toml.\n" },
    { "doc.txt",
      "# Welcome to Docs\n"
      "## Getting started\n"
      "Type text, then use \\ and a key to set the format:\n"
      "- \\ then 1 for a big heading\n"
      "- \\ then 2 for a smaller heading\n"
      "- \\ then - for a bullet line\n"
      "This is **bold text** inside a plain line.\n" },
    { "slides.txt",
      "Welcome to Slides\n"
      "- Press Ctrl+P to start the slideshow\n"
      "- Tab moves between the title and the bullets\n"
      "===\n"
      "Second Slide\n"
      "- One more point\n" },
};

static const struct {
    const char *name;
    const void *data;
    size_t len;
} builtin_bins[] = {
    { "prism.bmp", prism_sample_bmp, prism_sample_bmp_len },
    { "melody.wav", melody_sample_wav, melody_sample_wav_len },
    { "vid-01.pv1", prism_sample_vid_pv1_01, prism_sample_vid_pv1_01_len },
    { "vid-02.pv1", prism_sample_vid_pv1_02, prism_sample_vid_pv1_02_len },
    { "vid-03.pv1", prism_sample_vid_pv1_03, prism_sample_vid_pv1_03_len },
    { "vid-04.pv1", prism_sample_vid_pv1_04, prism_sample_vid_pv1_04_len },
    { "vid-05.pv1", prism_sample_vid_pv1_05, prism_sample_vid_pv1_05_len },
    { "vid-06.pv1", prism_sample_vid_pv1_06, prism_sample_vid_pv1_06_len },
    { "vid-07.pv1", prism_sample_vid_pv1_07, prism_sample_vid_pv1_07_len },
    { "vid-08.pv1", prism_sample_vid_pv1_08, prism_sample_vid_pv1_08_len },
    { "vid-09.pv1", prism_sample_vid_pv1_09, prism_sample_vid_pv1_09_len },
    { "vid-10.pv1", prism_sample_vid_pv1_10, prism_sample_vid_pv1_10_len },
    { "vid-11.pv1", prism_sample_vid_pv1_11, prism_sample_vid_pv1_11_len },
    { "vid-12.pv1", prism_sample_vid_pv1_12, prism_sample_vid_pv1_12_len },
    { "vid-13.pv1", prism_sample_vid_pv1_13, prism_sample_vid_pv1_13_len },
    { "vid-14.pv1", prism_sample_vid_pv1_14, prism_sample_vid_pv1_14_len },
    { "vid-15.pv1", prism_sample_vid_pv1_15, prism_sample_vid_pv1_15_len },
    { "vid-16.pv1", prism_sample_vid_pv1_16, prism_sample_vid_pv1_16_len },
    { "vid-17.pv1", prism_sample_vid_pv1_17, prism_sample_vid_pv1_17_len },
    { "vid-18.pv1", prism_sample_vid_pv1_18, prism_sample_vid_pv1_18_len },
    { "vid-19.pv1", prism_sample_vid_pv1_19, prism_sample_vid_pv1_19_len },
    { "vid-20.pv1", prism_sample_vid_pv1_20, prism_sample_vid_pv1_20_len },
    { "vid-21.pv1", prism_sample_vid_pv1_21, prism_sample_vid_pv1_21_len },
    { "vid-22.pv1", prism_sample_vid_pv1_22, prism_sample_vid_pv1_22_len },
    { "vid-23.pv1", prism_sample_vid_pv1_23, prism_sample_vid_pv1_23_len },
    { "vid-24.pv1", prism_sample_vid_pv1_24, prism_sample_vid_pv1_24_len },
    { "hello.elf", compat_guest_hello, compat_guest_hello_len },
    { "sample.drv", sample_driver_pkg, sample_driver_pkg_len },
    { "calc.elf", compat_guest_calc, compat_guest_calc_len },
    { "cat.elf", compat_guest_cat, compat_guest_cat_len },
    { "unsup.elf", compat_guest_unsup, compat_guest_unsup_len },
    { "evil.elf", compat_guest_evil, compat_guest_evil_len },
    { "loop.elf", compat_guest_loop, compat_guest_loop_len },
    { "libguest.so", compat_guest_libguest, compat_guest_libguest_len },
    { "maindyn.elf", compat_guest_maindyn, compat_guest_maindyn_len },
    { "evil2.elf", compat_guest_evil2, compat_guest_evil2_len },
    { "evil3.elf", compat_guest_evil3, compat_guest_evil3_len },
    { "cp.elf", compat_guest_cp, compat_guest_cp_len },
    { "wc.elf", compat_guest_wc, compat_guest_wc_len },
    { "gfxdemo.elf", compat_guest_gfxdemo, compat_guest_gfxdemo_len },
    { "netdemo.elf", compat_guest_netdemo, compat_guest_netdemo_len },
    { "hello-pe.exe", compat_guest_hello_pe, compat_guest_hello_pe_len },
};

void vfs_set_allocator(vfs_alloc_fn alloc, vfs_free_fn free_fn) {
    vfs_alloc = alloc;
    vfs_free = free_fn;
}

void vfs_init(void) {
    for (int i = 0; i < PRISM_VFS_MAX_FILES; i++) {
        if (files[i].used && files[i].data != NULL && vfs_free != NULL) {
            vfs_free(files[i].data);
        }
        files[i].used = 0;
        files[i].size = 0;
        files[i].data = NULL;
        files[i].name[0] = '\0';
    }
    if (vfs_alloc == NULL) {
        return; /* アロケータ未設定: ファイルは作れない */
    }
    for (size_t i = 0;
         i < sizeof(builtin_texts) / sizeof(builtin_texts[0]); i++) {
        seed(builtin_texts[i].name, builtin_texts[i].text);
    }
    for (size_t i = 0;
         i < sizeof(builtin_bins) / sizeof(builtin_bins[0]); i++) {
        seed_bytes(builtin_bins[i].name, builtin_bins[i].data,
                   builtin_bins[i].len);
    }
    vfs_seq++;
}

int vfs_is_builtin(const char *name) {
    if (name == NULL) {
        return 0;
    }
    for (size_t i = 0;
         i < sizeof(builtin_texts) / sizeof(builtin_texts[0]); i++) {
        if (vfs_eq(name, builtin_texts[i].name)) {
            return 1;
        }
    }
    for (size_t i = 0;
         i < sizeof(builtin_bins) / sizeof(builtin_bins[0]); i++) {
        if (vfs_eq(name, builtin_bins[i].name)) {
            return 1;
        }
    }
    return 0;
}

/* 組み込みファイルだけを正しい中身に戻す (回復モードの修復用)。
 * 無いものは作り、中身が違うものは書き直す。ユーザーのファイルは
 * 一切触らない。直したファイルの数を返す。アロケータ未設定の
 * 場合は -1。 */
int vfs_repair_builtins(void) {
    if (vfs_alloc == NULL) {
        return -1;
    }
    int fixed = 0;
    for (size_t i = 0;
         i < sizeof(builtin_texts) / sizeof(builtin_texts[0]); i++) {
        const char *name = builtin_texts[i].name;
        const char *want = builtin_texts[i].text;
        size_t want_len = vfs_len(want);
        size_t have_len = 0;
        const char *have = vfs_read_ptr(name, &have_len);
        int ok = 0;
        if (have != NULL && have_len == want_len) {
            ok = 1;
            for (size_t j = 0; j < want_len; j++) {
                if (have[j] != want[j]) {
                    ok = 0;
                    break;
                }
            }
        }
        if (!ok) {
            if (vfs_write(name, want, want_len) == 0) {
                fixed++;
            }
        }
    }
    for (size_t i = 0;
         i < sizeof(builtin_bins) / sizeof(builtin_bins[0]); i++) {
        const char *name = builtin_bins[i].name;
        const uint8_t *want = (const uint8_t *)builtin_bins[i].data;
        size_t want_len = builtin_bins[i].len;
        size_t have_len = 0;
        const char *have = vfs_read_ptr(name, &have_len);
        int ok = 0;
        if (have != NULL && have_len == want_len) {
            ok = 1;
            for (size_t j = 0; j < want_len; j++) {
                if ((uint8_t)have[j] != want[j]) {
                    ok = 0;
                    break;
                }
            }
        }
        if (!ok) {
            if (vfs_write(name, (const char *)want, want_len) == 0) {
                fixed++;
            }
        }
    }
    return fixed;
}

int vfs_file_count(void) {
    int n = 0;
    for (int i = 0; i < PRISM_VFS_MAX_FILES; i++) {
        if (files[i].used) {
            n++;
        }
    }
    return n;
}

const char *vfs_file_name(int index) {
    int seen = 0;
    for (int i = 0; i < PRISM_VFS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        if (seen == index) {
            return files[i].name;
        }
        seen++;
    }
    return NULL;
}

int vfs_exists(const char *name) {
    return find_slot(name) != NULL;
}

const char *vfs_read_ptr(const char *name, size_t *out_len) {
    vfs_file *f = find_slot(name);
    if (f == NULL) {
        return NULL;
    }
    if (out_len != NULL) {
        *out_len = f->size;
    }
    return f->data;
}

int vfs_write(const char *name, const char *data, size_t len) {
    if (!vfs_name_ok(name)) {
        return -3;
    }
    if (len > PRISM_VFS_MAX_FILE_SIZE) {
        return -1;
    }
    if (vfs_alloc == NULL) {
        return -4;
    }
    vfs_file *f = find_slot(name);
    if (f == NULL) {
        for (int i = 0; i < PRISM_VFS_MAX_FILES; i++) {
            if (!files[i].used) {
                f = &files[i];
                break;
            }
        }
        if (f == NULL) {
            return -2;
        }
    }
    /* 新しい中身を先に確保してから、古い中身を解放する。
     * 確保に失敗したら、既存ファイルはそのまま残す。 */
    char *buf = NULL;
    if (len > 0) {
        buf = (char *)vfs_alloc(len);
        if (buf == NULL) {
            return -4;
        }
        for (size_t i = 0; i < len; i++) {
            buf[i] = data[i];
        }
    }
    if (f->data != NULL && vfs_free != NULL) {
        vfs_free(f->data);
    }
    /* 新規スロットの場合のみここで使用中にして名前を入れる。
     * 既存ファイルの上書きでは名前は変わらない。 */
    if (!f->used) {
        f->used = 1;
        vfs_copy(f->name, name, sizeof(f->name));
    }
    f->data = buf;
    f->size = len;
    if (vfs_time_fn != NULL) {
        f->mtime = vfs_time_fn();
    }
    vfs_seq++;
    return 0;
}

int vfs_delete(const char *name) {
    vfs_file *f = find_slot(name);
    if (f == NULL) {
        return -1;
    }
    if (f->data != NULL && vfs_free != NULL) {
        vfs_free(f->data);
    }
    f->used = 0;
    f->size = 0;
    f->mtime = 0;
    f->data = NULL;
    f->name[0] = '\0';
    vfs_seq++;
    return 0;
}
