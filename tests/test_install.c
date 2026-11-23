/* PrismOS - host tests for the installer logic (shell/install.c).
 * Same pattern as the other host tests: a small CHECK helper,
 * no hardware, exit code 0 on success.
 */
#include <stdio.h>
#include <string.h>

#include "install.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(void) {
    /* ---- MBR parsing ---- */
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    CHECK(install_mbr_valid(mbr) == 0);
    install_partition parts[4];
    CHECK(install_mbr_partitions(mbr, parts, 4) == -1);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    CHECK(install_mbr_valid(mbr) == 1);
    CHECK(install_mbr_partitions(mbr, parts, 4) == 0);

    /* Entry 0: bootable FAT16, LBA 2048, 16384 sectors.
     * Entry 1: data partition 0x83, LBA 18432, 47104 sectors.
     * Entry 2: empty. Entry 3: type 0x82, LBA 65536, 8192. */
    mbr[446 + 0] = 0x80;
    mbr[446 + 4] = 0x0E;
    put_le32(mbr + 446 + 8, 2048);
    put_le32(mbr + 446 + 12, 16384);
    mbr[462 + 4] = 0x83;
    put_le32(mbr + 462 + 8, 18432);
    put_le32(mbr + 462 + 12, 47104);
    mbr[494 + 4] = 0x82;
    put_le32(mbr + 494 + 8, 65536);
    put_le32(mbr + 494 + 12, 8192);
    int n = install_mbr_partitions(mbr, parts, 4);
    CHECK(n == 3);
    CHECK(parts[0].bootable == 1);
    CHECK(parts[0].type == 0x0E);
    CHECK(parts[0].start_lba == 2048);
    CHECK(parts[0].sectors == 16384);
    CHECK(parts[1].bootable == 0);
    CHECK(parts[1].type == 0x83);
    CHECK(parts[1].start_lba == 18432);
    CHECK(parts[1].sectors == 47104);
    CHECK(parts[2].type == 0x82);
    CHECK(parts[2].start_lba == 65536);
    CHECK(parts[2].sectors == 8192);
    /* A small cap stops early. */
    n = install_mbr_partitions(mbr, parts, 2);
    CHECK(n == 2);
    CHECK(parts[1].type == 0x83);
    /* Bad signature is rejected even with entries present. */
    mbr[510] = 0x00;
    CHECK(install_mbr_valid(mbr) == 0);
    CHECK(install_mbr_partitions(mbr, parts, 4) == -1);

    /* ---- profile file selection ---- */
    CHECK(install_include_file("README.txt", 0) == 1);
    CHECK(install_include_file("prism.bmp", 0) == 1);
    CHECK(install_include_file("vid-07.bmp", 0) == 1);
    CHECK(install_include_file("note.txt", 0) == 1);
    CHECK(install_include_file("README.txt", 1) == 1);
    CHECK(install_include_file("VERSION", 1) == 1);
    CHECK(install_include_file("theme.toml", 1) == 1);
    CHECK(install_include_file("about.txt", 1) == 1);
    CHECK(install_include_file("settings.txt", 1) == 1);
    CHECK(install_include_file(".account", 1) == 1);
    CHECK(install_include_file("prism.bmp", 1) == 0);
    CHECK(install_include_file("melody.wav", 1) == 0);
    CHECK(install_include_file("vid-01.bmp", 1) == 0);
    CHECK(install_include_file("vid-12.bmp", 1) == 0);
    CHECK(install_include_file("note.txt", 1) == 0);
    CHECK(install_include_file("sheet.csv", 1) == 0);
    CHECK(install_include_file(NULL, 1) == 0);
    CHECK(install_include_file(NULL, 0) == 0);
    CHECK(strcmp(install_profile_name(0), "standard") == 0);
    CHECK(strcmp(install_profile_name(1), "minimal") == 0);

    /* ---- disk classification ---- */
    CHECK(install_classify(0, 0, 0) == INSTALL_DISK_NONE);
    CHECK(install_classify(0, 1, 1) == INSTALL_DISK_NONE);
    CHECK(install_classify(1, 0, 0) == INSTALL_DISK_BLANK);
    CHECK(install_classify(1, 1, 0) == INSTALL_DISK_DATA_ONLY);
    CHECK(install_classify(1, 0, 1) == INSTALL_DISK_BOOTABLE);
    CHECK(install_classify(1, 1, 1) == INSTALL_DISK_BOOTABLE);

    /* ---- step flow ---- */
    CHECK(install_step_next(INSTALL_STEP_WELCOME, 0, 0, 0) ==
          INSTALL_STEP_DISK);
    CHECK(install_step_next(INSTALL_STEP_DISK, 0, 0, 0) == -1);
    CHECK(install_step_next(INSTALL_STEP_DISK, 1, 0, 0) ==
          INSTALL_STEP_PROFILE);
    CHECK(install_step_next(INSTALL_STEP_PROFILE, 1, 0, 0) == -1);
    CHECK(install_step_next(INSTALL_STEP_PROFILE, 1, 1, 0) ==
          INSTALL_STEP_CONFIRM);
    CHECK(install_step_next(INSTALL_STEP_CONFIRM, 1, 1, 0) ==
          INSTALL_STEP_INSTALLING);
    CHECK(install_step_next(INSTALL_STEP_INSTALLING, 1, 1, 0) == -1);
    CHECK(install_step_next(INSTALL_STEP_DONE, 1, 1, 0) == -1);
    CHECK(install_step_next(INSTALL_STEP_FAILED, 1, 1, 0) == -1);
    CHECK(install_step_back(INSTALL_STEP_WELCOME) == -1);
    CHECK(install_step_back(INSTALL_STEP_DISK) == INSTALL_STEP_WELCOME);
    CHECK(install_step_back(INSTALL_STEP_PROFILE) == INSTALL_STEP_DISK);
    CHECK(install_step_back(INSTALL_STEP_CONFIRM) ==
          INSTALL_STEP_PROFILE);
    CHECK(install_step_back(INSTALL_STEP_INSTALLING) == -1);
    CHECK(install_step_back(INSTALL_STEP_DONE) == -1);
    CHECK(install_step_back(INSTALL_STEP_FAILED) ==
          INSTALL_STEP_CONFIRM);

    printf("install tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
