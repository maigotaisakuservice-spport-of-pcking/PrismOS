/* PrismOS — ML-KEM-512 KAT ハーネス (ホスト用)。
 *
 * scripts/pq-kat512.py から使われる。カーネルと同じ
 * shell/mlkem.c をリンクし、標準入出力でバイト列を
 * やり取りする:
 *   encaps: stdin = ek(800) || m(32)  → stdout = ct(768) || ss(32)
 *   decaps: stdin = dk(1632) || ct(768) → stdout = ss(32)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mlkem.h"

static size_t read_all(uint8_t *buf, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        size_t r = fread(buf + n, 1, cap - n, stdin);
        if (r == 0) {
            break;
        }
        n += r;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    static uint8_t in[1632 + 768];
    static uint8_t ct[MLKEM512_CT_LEN];
    static uint8_t ss[MLKEM_SS_LEN];
    if (strcmp(argv[1], "encaps") == 0) {
        if (read_all(in, MLKEM512_EK_LEN + 32) !=
            MLKEM512_EK_LEN + 32) {
            return 1;
        }
        if (mlkem512_encaps(in, in + MLKEM512_EK_LEN, ct, ss) != 0) {
            return 1;
        }
        fwrite(ct, 1, sizeof(ct), stdout);
        fwrite(ss, 1, sizeof(ss), stdout);
        return 0;
    }
    if (strcmp(argv[1], "decaps") == 0) {
        if (read_all(in, MLKEM512_DK_LEN + MLKEM512_CT_LEN) !=
            MLKEM512_DK_LEN + MLKEM512_CT_LEN) {
            return 1;
        }
        if (mlkem512_decaps(in, in + MLKEM512_DK_LEN, ss) != 0) {
            return 1;
        }
        fwrite(ss, 1, sizeof(ss), stdout);
        return 0;
    }
    return 2;
}
