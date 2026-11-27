#!/usr/bin/env python3
"""PrismOS bootable disk image builder.

Builds a disk the OS can boot from without a CD:
  - MBR with the Limine BIOS bootloader installed,
  - partition 1 (32MB, FAT16, bootable): limine-bios.sys,
    limine.conf and boot/prismos.elf,
  - partition 2 (the rest, type 0x83): an empty PRISMFS file
    system the kernel mounts as its data partition.

P1 is 32MB so a kernel update can hold the old and the new
kernel side by side (about 10MB each as of 2026-11) while
the update runs. (It was 16MB when kernels were ~4.6MB;
once the kernel passed ~7MB a second kernel no longer
fit next to the first, and kernel updates began failing
with "ran out of space".)
P2 is 31MB, still well above what snapshots need.

Layout (sectors, 512 bytes each):
  disk: 64MB = 131072 sectors
  P1: start 2048, size 65536 (32MB)
  P2: start 67584, size 63488 (31MB)
  PRISMFS2 inside P2: superblock at P2+1, table at P2+2..9,
  data from P2+10.

Usage: python3 scripts/make-disk.py [--disk PATH]
         [--boot-sectors N]
  --boot-sectors makes an old-style disk for testing (24576
  gives the old 12MB P1, which mformat formats as FAT12,
  so a kernel update must fail honestly as "not FAT16";
  12MB is the smallest FAT12 size that still holds the
  current ~10MB kernel).
  (exit code 0 = done; needs qemu-img, mtools, Limine)
"""

import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
LIMINE_CONF = os.path.join(ROOT, "limine.conf")
LIMINE_BIN = os.path.join(ROOT, "build", "limine", "limine")
LIMINE_BIOS_SYS = os.path.join(ROOT, "build", "limine", "limine-bios.sys")
DEFAULT_DISK = os.path.join(ROOT, "build", "prism-disk-boot.img")

DISK_SECTORS = 131072  # 64MB
BOOT_START = 2048
BOOT_SECTORS = 65536  # 32MB
DATA_START = 67584
DATA_SECTORS = DISK_SECTORS - DATA_START  # 63488


class DiskBuildFailure(Exception):
    pass


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise DiskBuildFailure("%s failed: %s%s" % (what, r.stdout, r.stderr))


def main():
    parser = argparse.ArgumentParser(description="PrismOS disk builder")
    parser.add_argument("--disk", default=DEFAULT_DISK)
    parser.add_argument("--boot-sectors", type=int, default=BOOT_SECTORS)
    args = parser.parse_args()
    disk = args.disk
    boot_sectors = args.boot_sectors
    data_start = BOOT_START + boot_sectors
    data_sectors = DISK_SECTORS - data_start
    if boot_sectors <= 0 or data_sectors < 10286:
        print("MISSING: bad --boot-sectors %d" % boot_sectors)
        return 1
    for path in (KERNEL, LIMINE_CONF, LIMINE_BIN, LIMINE_BIOS_SYS):
        if not os.path.exists(path):
            print("MISSING: %s not found (run `make` first)" % path)
            return 1

    fat_img = os.path.join(ROOT, "build", "prism-boot-fat.img")
    try:
        os.unlink(disk)
    except OSError:
        pass
    try:
        os.unlink(fat_img)
    except OSError:
        pass

    try:
        # 1. Blank 64MB disk.
        run(["qemu-img", "create", "-f", "raw", disk, "64M"], "qemu-img")
        print("disk: created 64MB disk image")

        # 2. FAT16 boot partition image with the bootloader files.
        run(["dd", "if=/dev/zero", "of=" + fat_img, "bs=512",
             "count=%d" % boot_sectors], "dd (FAT image)")
        run(["mformat", "-i", fat_img, "-T", str(boot_sectors),
             "-v", "PRISMBOOT", "::"], "mformat")
        run(["mmd", "-i", fat_img, "::/boot"], "mmd /boot")
        run(["mcopy", "-i", fat_img, LIMINE_BIOS_SYS,
             "::/limine-bios.sys"], "mcopy limine-bios.sys")
        run(["mcopy", "-i", fat_img, LIMINE_CONF,
             "::/limine.conf"], "mcopy limine.conf")
        run(["mcopy", "-i", fat_img, KERNEL,
             "::/boot/prismos.elf"], "mcopy kernel")
        print("disk: boot files copied to FAT partition")

        # 3. Copy the FAT image into the disk at the P1 offset.
        run(["dd", "if=" + fat_img, "of=" + disk, "bs=512",
             "seek=%d" % BOOT_START, "conv=notrunc"], "dd (FAT into disk)")

        # 4. MBR partition table + empty PRISMFS in P2.
        with open(disk, "r+b") as f:
            mbr = bytearray(512)
            mbr[446] = 0x80  # P1 bootable
            mbr[446 + 4] = 0x0E  # FAT16 LBA
            struct.pack_into("<I", mbr, 446 + 8, BOOT_START)
            struct.pack_into("<I", mbr, 446 + 12, boot_sectors)
            mbr[462 + 4] = 0x83  # P2: data partition
            struct.pack_into("<I", mbr, 462 + 8, data_start)
            struct.pack_into("<I", mbr, 462 + 12, data_sectors)
            mbr[510] = 0x55
            mbr[511] = 0xAA
            f.seek(0)
            f.write(mbr)
            sb = bytearray(512)
            sb[0:8] = b"PRISMFS2"
            struct.pack_into("<I", sb, 8, 2)   # version
            struct.pack_into("<I", sb, 12, 0)  # file count
            struct.pack_into("<I", sb, 16, 10)  # data start
            struct.pack_into("<I", sb, 20, 32)  # sectors per file
            struct.pack_into("<I", sb, 24, data_sectors)
            f.seek((data_start + 1) * 512)
            f.write(sb)
            f.seek((data_start + 2) * 512)
            f.write(bytes(512 * 8))  # empty file table
        print("disk: MBR and empty file system written")

        # 5. Install the Limine BIOS bootloader into the MBR.
        #    It preserves the partition table (verified below).
        run([LIMINE_BIN, "bios-install", disk], "limine bios-install")

        # 6. Verify: partition table, PRISMFS magic, boot files.
        with open(disk, "rb") as f:
            mbr = f.read(512)
            if mbr[510] != 0x55 or mbr[511] != 0xAA:
                raise DiskBuildFailure("MBR signature lost")
            p1 = mbr[446:462]
            if p1[4] != 0x0E or struct.unpack("<I", p1[8:12])[0] != BOOT_START:
                raise DiskBuildFailure("boot partition lost")
            p2 = mbr[462:478]
            if p2[4] != 0x83 or struct.unpack("<I", p2[8:12])[0] != data_start:
                raise DiskBuildFailure("data partition lost")
            f.seek((data_start + 1) * 512)
            if f.read(8) != b"PRISMFS2":
                raise DiskBuildFailure("file system magic lost")
        run(["mdir", "-i", "%s@@%d" % (disk, BOOT_START * 512),
             "::/boot"], "mdir (verify boot files)")
        print("DISK BUILD OK: %s" % disk)
        return 0
    except DiskBuildFailure as e:
        print("MISSING: %s" % e)
        return 1
    finally:
        try:
            os.unlink(fat_img)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
