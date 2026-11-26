/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The two ACPI facts core-unlock reads at run time, with the table layouts of the ACPI Specification 6.5: the
 * reset register of the FADT (section 5.2.9), and the enabled processors of the MADT (section 5.2.12), which is the
 * fallback count when the firmware has no MP Services protocol. The report option reads more: the list of tables,
 * more FADT fields and every processor entry of the MADT. Pure parsers: every table is reached through the
 * caller's map function, so the host tests give them tables in a buffer.
 */
#ifndef EFL_ACPI_H
#define EFL_ACPI_H

/* The bytes at a physical address, or NULL. Boot services map memory one to one, so the UEFI map is a cast. */
typedef const unsigned char *(*efl_map_fn)(void *context, unsigned long long address, unsigned long bytes);

struct efl_acpi {
	efl_map_fn map;
	void *context;
	unsigned long long rsdp;	/* the address of the RSDP, from the system configuration table */
};

/* The address spaces of a Generic Address Structure that a reset may use. */
#define EFL_ACPI_SPACE_MEMORY	0u
#define EFL_ACPI_SPACE_IO	1u
#define EFL_ACPI_SPACE_PCI	2u

struct efl_reset_register {
	unsigned space;			/* EFL_ACPI_SPACE_* */
	unsigned long long address;
	unsigned char value;
};

enum efl_acpi_result {
	EFL_ACPI_OK = 0,
	EFL_ACPI_NO_RSDP = 1,		/* no RSDP, or its signature or checksum is wrong */
	EFL_ACPI_NO_TABLE = 2,		/* no table of that signature with a correct checksum */
	EFL_ACPI_SHORT = 3,		/* the FADT ends before the reset value */
	EFL_ACPI_NOT_SUPPORTED = 4,	/* the FADT flag RESET_REG_SUP is clear */
	EFL_ACPI_BAD_REGISTER = 5	/* not an 8-bit register at bit 0, or address 0, or an unknown space */
};

/* The reset register and the value to write, as the FADT names them. */
enum efl_acpi_result efl_acpi_reset_register(const struct efl_acpi *acpi, struct efl_reset_register *out);

/* The enabled processors of the MADT: the Local APIC entries with the Enabled flag, or the x2APIC entries with it
 * when the table has no Local APIC entry. Firmware that lists a processor in both forms is not double counted. */
enum efl_acpi_result efl_acpi_madt_processors(const struct efl_acpi *acpi, unsigned *processors);

/* ---- the read-only report (docs/design/efi-core-unlock.md, "The report") ----------------------------- */

/* One table of the root table, or the DSDT that the FADT names. */
struct efl_acpi_table {
	char signature[5];
	char oem_id[7];
	char oem_table_id[9];
	unsigned revision;
	unsigned length;
	unsigned long long address;
	int checksum_ok;
	const unsigned char *bytes;	/* the whole table when the checksum is right, else NULL */
};

#define EFL_ACPI_MAX_TABLES	48u

/* Every table that the XSDT (or the RSDT) lists, in its order, then the DSDT of the FADT (X_DSDT when it is not 0).
 * A table whose header does not map or whose length is out of range is counted in *dropped. */
enum efl_acpi_result efl_acpi_tables(const struct efl_acpi *acpi, struct efl_acpi_table *out, unsigned capacity,
				   unsigned *count, unsigned *dropped);

/* The FADT fields of the report (ACPI 6.5 section 5.2.9). */
struct efl_acpi_fadt {
	unsigned revision;
	unsigned length;
	unsigned flags;
	unsigned p_lvl2_lat;		/* microseconds; above 100 means that C2 is not supported */
	unsigned p_lvl3_lat;		/* microseconds; above 1000 means that C3 is not supported */
	int has_reset;			/* the table is long enough for the reset register and value */
	unsigned reset_space, reset_width, reset_offset, reset_access;
	unsigned long long reset_address;
	unsigned reset_value;
};
enum efl_acpi_result efl_acpi_fadt_fields(const struct efl_acpi *acpi, struct efl_acpi_fadt *out);

/* One processor entry of the MADT: a Local APIC entry (type 0) or a Local x2APIC entry (type 9). */
struct efl_acpi_madt_cpu {
	unsigned type;
	unsigned uid;			/* the ACPI processor UID */
	unsigned apic_id;
	unsigned flags;			/* bit 0 Enabled, bit 1 Online Capable */
};
#define EFL_ACPI_MAX_MADT_CPUS	64u
enum efl_acpi_result efl_acpi_madt_list(const struct efl_acpi *acpi, struct efl_acpi_madt_cpu *out, unsigned capacity,
				      unsigned *count, unsigned *dropped);

#endif
