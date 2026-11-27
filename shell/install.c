/* PrismOS - installer logic (pure functions). See install.h. */
#include "install.h"
#include "string.h"

int install_mbr_valid(const uint8_t *mbr512) {
    if (mbr512 == NULL) {
        return 0;
    }
    return mbr512[510] == 0x55 && mbr512[511] == 0xAA;
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int install_mbr_partitions(const uint8_t *mbr512,
                           install_partition *out, int cap) {
    if (!install_mbr_valid(mbr512) || out == NULL || cap <= 0) {
        return -1;
    }
    int count = 0;
    for (int i = 0; i < INSTALL_PARTITION_COUNT; i++) {
        const uint8_t *e = mbr512 + 446 + i * 16;
        if (e[4] == 0) {
            continue; /* empty entry */
        }
        if (count >= cap) {
            break;
        }
        out[count].bootable = e[0] == 0x80;
        out[count].type = e[4];
        out[count].start_lba = read_le32(e + 8);
        out[count].sectors = read_le32(e + 12);
        count++;
    }
    return count;
}

int install_include_file(const char *name, int minimal) {
    if (name == NULL) {
        return 0;
    }
    if (!minimal) {
        return 1;
    }
    /* Minimal: the core system files only, plus the settings
     * and the account file when they exist. Everything else
     * (sample pictures, sounds, video frames, user files)
     * is left out. */
    if (strcmp(name, "README.txt") == 0 ||
        strcmp(name, "VERSION") == 0 ||
        strcmp(name, "theme.toml") == 0 ||
        strcmp(name, "about.txt") == 0 ||
        strcmp(name, "settings.txt") == 0 ||
        strcmp(name, ".account") == 0) {
        return 1;
    }
    return 0;
}

install_disk_kind install_classify(int present, int installed,
                                   int has_data_part) {
    if (!present) {
        return INSTALL_DISK_NONE;
    }
    if (has_data_part) {
        return INSTALL_DISK_BOOTABLE;
    }
    if (installed) {
        return INSTALL_DISK_DATA_ONLY;
    }
    return INSTALL_DISK_BLANK;
}

int install_step_next(install_step step, int has_disk, int profile_chosen,
                      int target) {
    (void)target;
    switch (step) {
    case INSTALL_STEP_WELCOME:
        return INSTALL_STEP_DISK;
    case INSTALL_STEP_DISK:
        if (!has_disk) {
            return -1;
        }
        return INSTALL_STEP_PROFILE;
    case INSTALL_STEP_PROFILE:
        if (!profile_chosen) {
            return -1;
        }
        return INSTALL_STEP_CONFIRM;
    case INSTALL_STEP_CONFIRM:
        return INSTALL_STEP_INSTALLING;
    case INSTALL_STEP_INSTALLING:
    case INSTALL_STEP_DONE:
    case INSTALL_STEP_FAILED:
    default:
        return -1;
    }
}

int install_step_back(install_step step) {
    switch (step) {
    case INSTALL_STEP_DISK:
        return INSTALL_STEP_WELCOME;
    case INSTALL_STEP_PROFILE:
        return INSTALL_STEP_DISK;
    case INSTALL_STEP_CONFIRM:
        return INSTALL_STEP_PROFILE;
    case INSTALL_STEP_FAILED:
        return INSTALL_STEP_CONFIRM;
    case INSTALL_STEP_WELCOME:
    case INSTALL_STEP_INSTALLING:
    case INSTALL_STEP_DONE:
    default:
        return -1;
    }
}

const char *install_profile_name(int minimal) {
    return minimal ? "minimal" : "standard";
}
