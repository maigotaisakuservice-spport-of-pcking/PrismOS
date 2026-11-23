/* PrismOS — store package format + installed list tests. */
#include "../shell/storepkg.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        checks++;                                          \
        if (!(cond)) {                                     \
            failures++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                 \
        }                                                  \
    } while (0)

int main(void) {
    static uint8_t pkg[8192];
    static const uint8_t payload[] = { 0x7F, 'E', 'L', 'F', 1, 2, 3 };
    static uint8_t sig[STOREPKG_SIG_LEN];
    for (size_t i = 0; i < sizeof(sig); i++) {
        sig[i] = (uint8_t)(i & 0xFF);
    }
    size_t plen = 0;
    CHECK(storepkg_build("calc", "calc.elf", "1.2", payload,
                         sizeof(payload), sig, pkg, sizeof(pkg),
                         &plen) == 0);
    CHECK(plen > sizeof(payload));
    CHECK(memcmp(pkg, "PRISMPK1", 8) == 0);

    storepkg_info info;
    const uint8_t *sig_out = NULL;
    const uint8_t *pl_out = NULL;
    CHECK(storepkg_parse(pkg, plen, &info, &sig_out, &pl_out) == 0);
    CHECK(strcmp(info.name, "calc") == 0);
    CHECK(strcmp(info.file, "calc.elf") == 0);
    CHECK(strcmp(info.version, "1.2") == 0);
    CHECK(info.payload_len == sizeof(payload));
    CHECK(memcmp(pl_out, payload, sizeof(payload)) == 0);
    CHECK(memcmp(sig_out, sig, sizeof(sig)) == 0);

    /* truncated / bad magic / bad names are refused */
    CHECK(storepkg_parse(pkg, plen - 1, &info, NULL, NULL) == -1);
    pkg[0] = 'X';
    CHECK(storepkg_parse(pkg, plen, &info, NULL, NULL) == -1);
    pkg[0] = 'P';
    CHECK(storepkg_build("bad/name", "x.elf", "1", payload,
                         sizeof(payload), sig, pkg, sizeof(pkg),
                         &plen) == -1);
    CHECK(storepkg_build("ok", ".hidden", "1", payload,
                         sizeof(payload), sig, pkg, sizeof(pkg),
                         &plen) == -1);
    CHECK(storepkg_build("ok", "x.elf", "1", payload,
                         sizeof(payload), sig, pkg, 16, &plen) ==
          -1);

    /* installed list */
    char text[1024];
    text[0] = '\0';
    char out[1024];
    CHECK(store_installed_add(text, 0, "calc", "1.2", "calc.elf",
                              out, sizeof(out)) == 0);
    strcpy(text, out);
    CHECK(store_installed_contains(text, strlen(text), "calc") == 1);
    CHECK(store_installed_contains(text, strlen(text), "cal") == 0);
    CHECK(store_installed_contains(text, strlen(text), "calc2") == 0);
    CHECK(store_installed_add(text, strlen(text), "game", "0.9",
                              "game.elf", out, sizeof(out)) == 0);
    strcpy(text, out);
    /* re-adding replaces the version */
    CHECK(store_installed_add(text, strlen(text), "calc", "1.3",
                              "calc.elf", out, sizeof(out)) == 0);
    strcpy(text, out);
    CHECK(strstr(text, "calc 1.3 calc.elf") != NULL);
    CHECK(strstr(text, "calc 1.2") == NULL);
    char file[64];
    CHECK(store_installed_remove(text, strlen(text), "calc", out,
                                 sizeof(out), file,
                                 sizeof(file)) == 0);
    CHECK(strcmp(file, "calc.elf") == 0);
    strcpy(text, out);
    CHECK(store_installed_contains(text, strlen(text), "calc") == 0);
    CHECK(store_installed_contains(text, strlen(text), "game") == 1);
    CHECK(store_installed_remove(text, strlen(text), "calc", out,
                                 sizeof(out), file,
                                 sizeof(file)) == -1);

    printf("storepkg tests: %d checks, %d failures\n", checks,
           failures);
    return failures != 0;
}
