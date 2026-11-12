/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A name scan of one DSDT or SSDT, for the read-only report (docs/design/efi-core-unlock.md, "The report"). It is
 * not an AML interpreter: it executes nothing and resolves no reference. It follows the encodings of the ACPI
 * Specification 6.5 chapter 20 that open a named scope (Scope, Device, Processor, ThermalZone, PowerResource) and
 * the two that define a name (Name, Method), and it skips the data objects whose bytes could look like a name
 * (Buffer, String, the integer prefixes). Each power management name that it finds is given to the scope that
 * encloses its definition.
 */
#ifndef CU_AML_H
#define CU_AML_H
#include <stddef.h>

/* The processor power management names of ACPI 6.5 chapter 8 that the report looks for. One bit each. */
enum cu_aml_power_name {
	CU_AML_CST = 0, CU_AML_CSD, CU_AML_PSS, CU_AML_PCT, CU_AML_PPC, CU_AML_PSD, CU_AML_CPC, CU_AML_LPI,
	CU_AML_POWER_NAMES
};
const char *cu_aml_power_name_text(enum cu_aml_power_name name);

/* What an object of the list is. */
enum cu_aml_kind {
	CU_AML_PROCESSOR = 0,	/* a Processor object (deprecated in ACPI 6.5, still in many tables) */
	CU_AML_DEVICE_CPU,	/* a Device with _HID "ACPI0007", the processor device */
	CU_AML_DEVICE,		/* another Device that defines a power management name */
	CU_AML_SCOPE		/* a Scope statement that defines a power management name */
};
const char *cu_aml_kind_text(enum cu_aml_kind kind);

#define CU_AML_PATH_CHARS	64u	/* "\_PR_.C000" form: the name segments with their trailing underscores */
#define CU_AML_MAX_OBJECTS	64u
#define CU_AML_MAX_DEPTH	16u
#define CU_AML_MAX_SEGMENTS	8u

struct cu_aml_object {
	char path[CU_AML_PATH_CHARS];
	enum cu_aml_kind kind;
	unsigned names;		/* bit (1 << cu_aml_power_name) for each name defined directly in this scope */
};

/* The thermal and fan names that the report counts. The counts are occurrences of the name's bytes anywhere in the
 * table, definitions and references alike. */
enum cu_aml_count {
	CU_AML_TZ = 0, CU_AML_TMP, CU_AML_CRT, CU_AML_PSV, CU_AML_AC0, CU_AML_FST, CU_AML_FAN_STRING, CU_AML_FAN_EISAID,
	CU_AML_COUNTS
};
const char *cu_aml_count_text(enum cu_aml_count count);

struct cu_aml_scan {
	unsigned objects;
	unsigned dropped;		/* objects that did not fit in the list */
	unsigned processor_objects;	/* every Processor object, also the ones that did not fit */
	unsigned thermal_zones;		/* ThermalZone objects */
	unsigned malformed;		/* scope openings whose length or name did not parse; the scan went on */
	unsigned counts[CU_AML_COUNTS];
	struct cu_aml_object object[CU_AML_MAX_OBJECTS];
};

/* Scans one table (its whole bytes, the 36-byte header included). Returns 0 when the bytes are not a DSDT or SSDT.
 * The list holds the processor objects, the processor devices, and every other scope that defines one of the power
 * management names, in the order of the table. */
int cu_aml_scan(const unsigned char *table, size_t bytes, struct cu_aml_scan *out);

/* The 32-bit EISA ID of a seven-character ID such as "PNP0C0B", as the ASL macro EISAID() encodes it (ACPI 6.5
 * section 19.6.35). 0 when the text is not of that form. */
unsigned cu_aml_eisaid(const char *id);

#endif
