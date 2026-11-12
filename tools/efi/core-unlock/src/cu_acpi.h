/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The two ACPI facts core-unlock reads at run time, with the table layouts of the ACPI Specification 6.5: the
 * reset register of the FADT (section 5.2.9), and the enabled processors of the MADT (section 5.2.12), which is the
 * fallback count when the firmware has no MP Services protocol. Pure parsers: every table is reached through the
 * caller's map function, so test/host_test.c gives them tables in a buffer.
 */
#ifndef CU_ACPI_H
#define CU_ACPI_H

/* The bytes at a physical address, or NULL. Boot services map memory one to one, so the UEFI map is a cast. */
typedef const unsigned char *(*cu_map_fn)(void *context, unsigned long long address, unsigned long bytes);

struct cu_acpi {
	cu_map_fn map;
	void *context;
	unsigned long long rsdp;	/* the address of the RSDP, from the system configuration table */
};

/* The address spaces of a Generic Address Structure that a reset may use. */
#define CU_ACPI_SPACE_MEMORY	0u
#define CU_ACPI_SPACE_IO	1u
#define CU_ACPI_SPACE_PCI	2u

struct cu_reset_register {
	unsigned space;			/* CU_ACPI_SPACE_* */
	unsigned long long address;
	unsigned char value;
};

enum cu_acpi_result {
	CU_ACPI_OK = 0,
	CU_ACPI_NO_RSDP = 1,		/* no RSDP, or its signature or checksum is wrong */
	CU_ACPI_NO_TABLE = 2,		/* no table of that signature with a correct checksum */
	CU_ACPI_SHORT = 3,		/* the FADT ends before the reset value */
	CU_ACPI_NOT_SUPPORTED = 4,	/* the FADT flag RESET_REG_SUP is clear */
	CU_ACPI_BAD_REGISTER = 5	/* not an 8-bit register at bit 0, or address 0, or an unknown space */
};

/* The reset register and the value to write, as the FADT names them. */
enum cu_acpi_result cu_acpi_reset_register(const struct cu_acpi *acpi, struct cu_reset_register *out);

/* The enabled processors of the MADT: the Local APIC entries with the Enabled flag, or the x2APIC entries with it
 * when the table has no Local APIC entry. Firmware that lists a processor in both forms is not double counted. */
enum cu_acpi_result cu_acpi_madt_processors(const struct cu_acpi *acpi, unsigned *processors);

#endif
