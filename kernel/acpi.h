/* ACPI power management: FADT parsing, S3 sleep and
 * S5 power-off.
 *
 * The FADT is found by walking the RSDT/XSDT from the
 * RSDP (the same walk the SMP code does for the MADT).
 * The sleep type values for S3/S5 come from the \_S3
 * and \_S5 packages in the DSDT, which is what those
 * packages exist for; entering a state is one write of
 * (SLP_TYP << 10) | SLP_EN to the PM1a control register.
 *
 * Battery: the ACPI namespace is scanned for a battery
 * device (PNP0C0A). QEMU's default machine has none, in
 * which case acpi_battery_present() honestly returns 0
 * and no charge information is invented. */
#ifndef PRISM_ACPI_H
#define PRISM_ACPI_H

#include <stdint.h>

/* Walk the ACPI tables. Returns 0 when a FADT was found. */
int acpi_init(uint64_t rsdp_addr, uint64_t hhdm_offset);

/* 1 when the firmware offers S3 (a \_S3 package exists). */
int acpi_s3_available(void);
/* SLP_TYP values from the DSDT packages, or -1. */
int acpi_s3_slp_typ(void);
int acpi_s5_slp_typ(void);
/* PM1a_CNT I/O port, or 0 when no FADT was found. */
uint16_t acpi_pm1_cnt_port(void);
/* 1 when the DSDT names a battery device (PNP0C0A). */
int acpi_battery_present(void);

/* Enter S3. Returns after the machine wakes (the caller
 * prints the "resumed" line). -1 when S3 is unavailable. */
int acpi_enter_s3(void);

/* Enter S5 (power off). Does not return on success; falls
 * back to the QEMU/Bochs shutdown port when no FADT/S5
 * is available, and returns -1 if even that is absent. */
int acpi_poweroff(void);

#endif /* PRISM_ACPI_H */
