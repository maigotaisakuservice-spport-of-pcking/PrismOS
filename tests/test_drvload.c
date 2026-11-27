/*
 * 後付けドライバローダのホストテスト。実物のサンプルパッケージ
 * (build/sample_hello.o) を読み込ませ、再配置と API 解決が
 * 本当に働くこと (ドライバが API 経由でファイルを書く)、
 * 形式違いと未解決シンボルが拒否されることを確かめる。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "../kernel/drvload.h"

/* ---- スタブ ---- */
static char g_written_name[64];
static char g_written_data[256];
static int g_write_count;

/* ホストのヒープは NX なので、ドライバのコードを置く確保は
 * 実行可能な mmap で代用する (カーネルのヒープは実行可能)。 */
#include <sys/mman.h>
void *kmalloc(size_t n) {
    /* -no-pie のテスト本体 (0x400000 付近) から PC32 相対で
     * 届く範囲 (< 2GB) に置く。カーネルではヒープがカーネル
     * 像の直後にあり、同じ近接性が成り立つ。 */
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void kfree(void *p) {
    (void)p; /* テスト中は解放しない (サイズを記録していない) */
}
void serial_write(const char *s) { (void)s; }
void serial_write_line(const char *s) { (void)s; }

int vfs_write(const char *name, const char *data, size_t len) {
    snprintf(g_written_name, sizeof(g_written_name), "%s", name);
    size_t n = len < sizeof(g_written_data) - 1 ? len
                                                : sizeof(g_written_data) - 1;
    memcpy(g_written_data, data, n);
    g_written_data[n] = '\0';
    g_write_count++;
    return 0;
}

const char *vfs_read_ptr(const char *name, size_t *len_out) {
    (void)name;
    (void)len_out;
    return NULL;
}

/* rtc スタブ (prism_drv_tick 用) */
#include "../kernel/rtc.h"
int rtc_read(prism_rtc_time *out) {
    memset(out, 0, sizeof(*out));
    out->year = 26;
    out->month = 1;
    out->day = 1;
    return 0;
}
uint32_t rtc_epoch_seconds(const prism_rtc_time *t) {
    (void)t;
    return 1767225600u;
}

static int checks, failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

int main(void) {
    size_t len = 0;
    uint8_t *pkg = read_file("build/sample_hello.o", &len);
    CHECK(pkg != NULL);
    if (pkg == NULL) {
        printf("drvload tests: %d checks, %d failures\n", checks, failures);
        return 1;
    }
    int rc = drv_load("sample.drv", pkg, len);
    CHECK(rc == 0);
    CHECK(drv_loaded_count() == 1);
    CHECK(drv_loaded_name(0) != NULL &&
          strcmp(drv_loaded_name(0), "sample.drv") == 0);
    /* ドライバの init が API 経由でファイルを書いた。 */
    CHECK(g_write_count == 1);
    CHECK(strcmp(g_written_name, "driver-hello.txt") == 0);
    CHECK(strcmp(g_written_data, "Hello from a loadable driver.\n") == 0);

    /* 形式違いは -1。 */
    CHECK(drv_load("junk", (const uint8_t *)"not an elf", 10) == -1);
    /* 途中で切れた ELF は -2 (または -1)。 */
    CHECK(drv_load("trunc", pkg, 40) < 0);

    /* 未解決シンボルは -3。 */
    size_t blen = 0;
    uint8_t *bad = read_file("build/sample_bad.o", &blen);
    CHECK(bad != NULL);
    if (bad != NULL) {
        CHECK(drv_load("bad.drv", bad, blen) == -3);
        free(bad);
    }

    /* --- 細工 ET_REL の境界検証 (監査 F2) ---
     * 正規パッケージのコピーを書き換え、すべて -2 で
     * 拒否されること (確保域外への読み書きが起きない
     * こと) を確かめる。 */
    {
        uint8_t *c1 = malloc(len);
        memcpy(c1, pkg, len);
        Elf64_Ehdr *eh = (Elf64_Ehdr *)c1;
        Elf64_Shdr *sh = (Elf64_Shdr *)(c1 + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_SYMTAB) {
                sh[i].sh_entsize = 16; /* 構造体サイズと不一致 */
            }
        }
        CHECK(drv_load("craft-symtab", c1, len) == -2);
        free(c1);
    }
    {
        uint8_t *c2 = malloc(len);
        memcpy(c2, pkg, len);
        Elf64_Ehdr *eh = (Elf64_Ehdr *)c2;
        Elf64_Shdr *sh = (Elf64_Shdr *)(c2 + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_RELA) {
                sh[i].sh_entsize = 16;
                break;
            }
        }
        CHECK(drv_load("craft-relaent", c2, len) == -2);
        free(c2);
    }
    {
        uint8_t *c3 = malloc(len);
        memcpy(c3, pkg, len);
        Elf64_Ehdr *eh = (Elf64_Ehdr *)c3;
        Elf64_Shdr *sh = (Elf64_Shdr *)(c3 + eh->e_shoff);
        int done = 0;
        for (int i = 0; i < eh->e_shnum && !done; i++) {
            if (sh[i].sh_type == SHT_RELA && sh[i].sh_size > 0) {
                Elf64_Rela *re = (Elf64_Rela *)(c3 + sh[i].sh_offset);
                re[0].r_offset = 0x100000; /* 対象セクションの外 */
                done = 1;
            }
        }
        CHECK(done == 1);
        CHECK(drv_load("craft-roffset", c3, len) == -2);
        free(c3);
    }
    {
        uint8_t *c4 = malloc(len);
        memcpy(c4, pkg, len);
        Elf64_Ehdr *eh = (Elf64_Ehdr *)c4;
        Elf64_Shdr *sh = (Elf64_Shdr *)(c4 + eh->e_shoff);
        int done = 0;
        for (int i = 0; i < eh->e_shnum && !done; i++) {
            if (sh[i].sh_type == SHT_NOBITS) {
                sh[i].sh_size = 0xFFFFFFF0u; /* ファイル長を超過 */
                done = 1;
            }
        }
        CHECK(done == 1);
        CHECK(drv_load("craft-bss", c4, len) == -2);
        free(c4);
    }
    /* 細工は 1 件も読み込まれていない。 */
    CHECK(drv_loaded_count() == 1);
    free(pkg);
    printf("drvload tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
