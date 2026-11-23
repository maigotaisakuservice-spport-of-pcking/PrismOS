/* PrismOS - installer logic (pure functions, no hardware).
 *
 * The parts of the installer that can be tested on the host
 * without a disk: reading the partition table out of an MBR
 * sector, deciding which files belong to an install profile,
 * classifying the attached disk, and the installer's step
 * flow (welcome -> disk -> profile -> confirm -> install
 * -> done). The screen app (desktop.c) and the disk writer
 * (persist.c) use these; they own all hardware access.
 *
 * Profiles: "standard" installs every file in the file
 * system; "minimal" installs only the core system files
 * (README.txt, VERSION, theme.toml, about.txt) plus
 * settings.txt and the account file when they exist, so the
 * installed system is the browser, the file manager and
 * settings only.
 */
#ifndef PRISM_INSTALL_H
#define PRISM_INSTALL_H

#include <stddef.h>
#include <stdint.h>

/* One MBR partition-table entry (16 bytes at 446 + i * 16). */
typedef struct {
    int bootable;      /* 1 = the boot flag (0x80) is set */
    uint8_t type;      /* partition type byte, e.g. 0x83, 0x0E */
    uint32_t start_lba; /* first sector of the partition */
    uint32_t sectors;  /* size of the partition in sectors */
} install_partition;

#define INSTALL_MBR_SIZE 512
#define INSTALL_PARTITION_COUNT 4

/* 1 = the sector ends with the 0x55AA signature. */
int install_mbr_valid(const uint8_t *mbr512);

/* Parse the four partition entries out of an MBR sector.
 * Returns the number of entries written (0-4), or -1 when the
 * sector has no valid signature. Empty entries (type 0) are
 * skipped, so the result holds only real partitions. */
int install_mbr_partitions(const uint8_t *mbr512,
                           install_partition *out, int cap);

/* 1 = this file belongs to the install profile,
 * 0 = it is left out. minimal = 1 keeps only the core files
 * (see the header comment); minimal = 0 keeps everything. */
int install_include_file(const char *name, int minimal);

/* What the attached disk looks like, from the installer's
 * point of view. */
typedef enum {
    INSTALL_DISK_NONE = 0,  /* no ATA disk at all */
    INSTALL_DISK_BLANK,     /* disk present, no valid MBR */
    INSTALL_DISK_DATA_ONLY, /* installed data disk, no boot partition */
    INSTALL_DISK_BOOTABLE   /* boot partition + data partition */
} install_disk_kind;

/* Classify a disk: present = an ATA disk was found,
 * installed = the disk already holds an installed system,
 * has_data_part = the MBR has a data partition (a bootable
 * disk made by make-disk.py). */
install_disk_kind install_classify(int present, int installed,
                                   int has_data_part);

/* The installer's steps, in order. */
typedef enum {
    INSTALL_STEP_WELCOME = 0,
    INSTALL_STEP_DISK,
    INSTALL_STEP_PROFILE,
    INSTALL_STEP_CONFIRM,
    INSTALL_STEP_INSTALLING,
    INSTALL_STEP_DONE,
    INSTALL_STEP_FAILED
} install_step;

/* The next step when the user presses Enter, or -1 when the
 * step cannot advance yet (no disk on the disk step, nothing
 * chosen on the profile step, install not confirmed). The
 * target argument is the install target chosen on the disk
 * step: 0 = automatic, 1 = the data partition. */
int install_step_next(install_step step, int has_disk, int profile_chosen,
                      int target);

/* The previous step when the user presses Esc, or -1 when
 * there is no step back (welcome, installing, done). */
int install_step_back(install_step step);

/* "standard" or "minimal". */
const char *install_profile_name(int minimal);

#endif /* PRISM_INSTALL_H */
