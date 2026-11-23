/* PrismOS — FDE の鍵ラップ。fde.h と docs/FDE_DESIGN.md。 */
#include "fde.h"

#include "crypto.h"
#include "string.h"

/* HMAC-SHA-256 の先頭 mac_len バイト。persist.c の
 * persist_secret_mac と同じ組み立て (ipad/opad)。 */
static void fde_hmac(const uint8_t *key, const uint8_t *iv,
                     const uint8_t *ct, size_t ct_len,
                     uint8_t *mac_out, size_t mac_len) {
    uint8_t pad[64];
    uint8_t inner_hash[32];
    crypto_sha256_ctx ctx;
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x36);
    }
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, pad, 64);
    crypto_sha256_update(&ctx, iv, 16);
    crypto_sha256_update(&ctx, ct, ct_len);
    crypto_sha256_final(&ctx, inner_hash);
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x5C);
    }
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, pad, 64);
    crypto_sha256_update(&ctx, inner_hash, 32);
    uint8_t full[32];
    crypto_sha256_final(&ctx, full);
    for (size_t i = 0; i < mac_len; i++) {
        mac_out[i] = full[i];
    }
}

int fde_wrap_key(const char *passphrase, const uint8_t *salt,
                 uint32_t iterations, const uint8_t *iv,
                 const uint8_t *master_key, fde_header *out) {
    if (passphrase == NULL || salt == NULL || iv == NULL ||
        master_key == NULL || out == NULL || iterations == 0 ||
        iterations > FDE_MAX_ITERATIONS) {
        return -1;
    }
    uint8_t kek[32];
    crypto_kdf(passphrase, salt, FDE_SALT_LEN, iterations, kek);
    for (int i = 0; i < FDE_SALT_LEN; i++) {
        out->salt[i] = salt[i];
    }
    out->iterations = iterations;
    for (int i = 0; i < 16; i++) {
        out->iv[i] = iv[i];
    }
    crypto_aes256_ctr(kek, iv, master_key, out->wrapped,
                      FDE_MASTER_KEY_LEN);
    fde_hmac(kek, iv, out->wrapped, FDE_MASTER_KEY_LEN, out->mac,
             sizeof(out->mac));
    for (int i = 0; i < 32; i++) {
        kek[i] = 0;
    }
    return 0;
}

int fde_unwrap_key(const char *passphrase, const fde_header *hdr,
                   uint8_t *master_key_out) {
    if (passphrase == NULL || hdr == NULL ||
        master_key_out == NULL || hdr->iterations == 0 ||
        hdr->iterations > FDE_MAX_ITERATIONS) {
        return -1;
    }
    uint8_t kek[32];
    crypto_kdf(passphrase, hdr->salt, FDE_SALT_LEN,
               hdr->iterations, kek);
    uint8_t mac[16];
    fde_hmac(kek, hdr->iv, hdr->wrapped, FDE_MASTER_KEY_LEN, mac,
             sizeof(mac));
    int diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= mac[i] ^ hdr->mac[i];
    }
    if (diff != 0) {
        for (int i = 0; i < 32; i++) {
            kek[i] = 0;
        }
        return -1;
    }
    crypto_aes256_ctr(kek, hdr->iv, hdr->wrapped, master_key_out,
                      FDE_MASTER_KEY_LEN);
    for (int i = 0; i < 32; i++) {
        kek[i] = 0;
    }
    return 0;
}

void fde_sector_crypt(const uint8_t *master_key, uint64_t lba,
                      const uint8_t *in, uint8_t *out) {
    /* IV の材料: マスター鍵 (32) || LBA (BE64) || ブロック
     * 番号 (BE32、セクタ単位なので常に 0)。 */
    uint8_t material[32 + 8 + 4];
    for (int i = 0; i < 32; i++) {
        material[i] = master_key[i];
    }
    for (int i = 0; i < 8; i++) {
        material[32 + i] =
            (uint8_t)((lba >> ((7 - i) * 8)) & 0xFF);
    }
    for (int i = 0; i < 4; i++) {
        material[40 + i] = 0;
    }
    uint8_t hash[32];
    crypto_sha256(material, sizeof(material), hash);
    crypto_aes256_ctr(master_key, hash, in, out, 512);
    for (int i = 0; i < 32; i++) {
        hash[i] = 0;
    }
}

static void fde_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t fde_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fde_header_serialize(const fde_header *hdr, uint8_t *sector) {
    if (hdr == NULL || sector == NULL) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        sector[i] = 0;
    }
    const char magic[8] = {'P', 'R', 'I', 'S', 'M', 'F', 'K', '1'};
    for (int i = 0; i < 8; i++) {
        sector[i] = (uint8_t)magic[i];
    }
    for (int i = 0; i < FDE_SALT_LEN; i++) {
        sector[8 + i] = hdr->salt[i];
    }
    fde_put32(sector + 24, hdr->iterations);
    for (int i = 0; i < 16; i++) {
        sector[28 + i] = hdr->iv[i];
    }
    for (int i = 0; i < FDE_MASTER_KEY_LEN; i++) {
        sector[44 + i] = hdr->wrapped[i];
    }
    for (int i = 0; i < 16; i++) {
        sector[76 + i] = hdr->mac[i];
    }
    return 0;
}

int fde_header_parse(const uint8_t *sector, fde_header *out) {
    if (sector == NULL || out == NULL) {
        return -1;
    }
    const char magic[8] = {'P', 'R', 'I', 'S', 'M', 'F', 'K', '1'};
    for (int i = 0; i < 8; i++) {
        if (sector[i] != (uint8_t)magic[i]) {
            return -1;
        }
    }
    for (int i = 0; i < FDE_SALT_LEN; i++) {
        out->salt[i] = sector[8 + i];
    }
    out->iterations = fde_get32(sector + 24);
    if (out->iterations == 0 ||
        out->iterations > FDE_MAX_ITERATIONS) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        out->iv[i] = sector[28 + i];
    }
    for (int i = 0; i < FDE_MASTER_KEY_LEN; i++) {
        out->wrapped[i] = sector[44 + i];
    }
    for (int i = 0; i < 16; i++) {
        out->mac[i] = sector[76 + i];
    }
    return 0;
}
