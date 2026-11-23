/* PrismOS — 更新マニフェストの解析とバージョン比較 (純粋関数)。 */
#include "update.h"

#include "crypto.h"
#include "mldsa.h"
#include "pq_key.h"
#include "string.h"

static int starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t len) {
    size_t i = 0;
    while (i + 1 < cap && i < len && src[i] != '\0' && src[i] != '\r') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int update_hex_to_bytes(const char *hex, uint8_t *out32) {
    if (hex == NULL || out32 == NULL) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_val(hex[i * 2]);
        if (hi < 0) {
            return -1; /* lo を評価しない (文字列末尾の先を読まない) */
        }
        int lo = hex_val(hex[i * 2 + 1]);
        if (lo < 0) {
            return -1;
        }
        out32[i] = (uint8_t)((hi << 4) | lo);
    }
    if (hex[64] != '\0' && hex[64] != '\r' && hex[64] != '\n' &&
        hex[64] != ' ') {
        return -1;
    }
    return 0;
}

void update_bytes_to_hex(const uint8_t *bytes, char *hex_out) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2] = HEX[(bytes[i] >> 4) & 0xF];
        hex_out[i * 2 + 1] = HEX[bytes[i] & 0xF];
    }
    hex_out[64] = '\0';
}

void update_sha256_hex(const uint8_t *data, size_t len, char *hex_out) {
    uint8_t hash[32];
    crypto_sha256(data, len, hash);
    update_bytes_to_hex(hash, hex_out);
}

int update_verify_manifest_sig(const uint8_t *manifest, size_t manifest_len,
                               const uint8_t *sig, size_t sig_len) {
    if (manifest == NULL || sig == NULL) {
        return -1;
    }
    if (sig_len != MLDSA44_SIG_LEN) {
        return -1;
    }
    return mldsa44_verify(PQ_UPDATE_PK, manifest, manifest_len, sig);
}

int update_parse_manifest(const char *text, size_t len,
                          update_manifest *out) {
    if (text == NULL || out == NULL) {
        return -1;
    }
    out->version[0] = '\0';
    copy_field(out->channel, sizeof(out->channel), "stable", 6);
    copy_field(out->type, sizeof(out->type), "feature", 7);
    out->base[0] = '\0';
    out->file_count = 0;
    out->has_kernel = 0;
    for (int k = 0; k < 32; k++) {
        out->kernel_sha256[k] = 0;
    }
    size_t i = 0;
    while (i < len) {
        /* 1 行 (最大 127 文字) を取り出す。 */
        char line[128];
        size_t n = 0;
        while (i < len && text[i] != '\n' && n + 1 < sizeof(line)) {
            line[n++] = text[i++];
        }
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i < len) {
            i++; /* 改行を飛ばす */
        }
        line[n] = '\0';
        if (starts_with(line, "version=")) {
            copy_field(out->version, sizeof(out->version), line + 8,
                       n - 8);
        } else if (starts_with(line, "channel=")) {
            copy_field(out->channel, sizeof(out->channel), line + 8,
                       n - 8);
        } else if (starts_with(line, "type=")) {
            copy_field(out->type, sizeof(out->type), line + 5, n - 5);
        } else if (starts_with(line, "base=")) {
            copy_field(out->base, sizeof(out->base), line + 5, n - 5);
        } else if (starts_with(line, "kernel ")) {
            /* kernel <sha256 64 桁> */
            uint8_t hash[32];
            if (update_hex_to_bytes(line + 7, hash) != 0) {
                return -2;
            }
            out->has_kernel = 1;
            for (int k = 0; k < 32; k++) {
                out->kernel_sha256[k] = hash[k];
            }
        } else if (starts_with(line, "file ")) {
            /* file <名前> <sha256 64 桁> */
            const char *p = line + 5;
            char name[UPDATE_NAME_MAX + 1];
            size_t ni = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                   ni < UPDATE_NAME_MAX) {
                name[ni++] = *p++;
            }
            name[ni] = '\0';
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (ni == 0 || out->file_count >= UPDATE_MAX_FILES) {
                return -2;
            }
            uint8_t hash[32];
            if (update_hex_to_bytes(p, hash) != 0) {
                return -2;
            }
            update_file *f = &out->files[out->file_count];
            size_t j = 0;
            while (name[j] != '\0') {
                f->name[j] = name[j];
                j++;
            }
            f->name[j] = '\0';
            for (int k = 0; k < 32; k++) {
                f->sha256[k] = hash[k];
            }
            out->file_count++;
        }
        /* 知らない行は無視する。 */
    }
    if (out->version[0] == '\0') {
        return -1;
    }
    return 0;
}

/* 数字の区切り (最大 4 区切り) を読む。 */
static int next_num(const char **p, int *out) {
    const char *s = *p;
    int v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    *p = s;
    return 1;
}

int update_version_cmp(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    const char *pa = a;
    const char *pb = b;
    for (int seg = 0; seg < 4; seg++) {
        int na = 0;
        int nb = 0;
        int ha = next_num(&pa, &na);
        int hb = next_num(&pb, &nb);
        if (!ha && !hb) {
            break;
        }
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
        if (*pa == '.') {
            pa++;
        }
        if (*pb == '.') {
            pb++;
        }
    }
    /* 数字が同じなら、接尾辞 (-beta など) がある方が古い。 */
    int sa = (*pa == '-') ? 1 : 0;
    int sb = (*pb == '-') ? 1 : 0;
    if (sa != sb) {
        return sa ? -1 : 1;
    }
    return 0;
}
