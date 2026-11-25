/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A name scan of one DSDT or SSDT, for the read-only report (docs/design/efi-core-unlock.md, "The report") and for
 * the ACPI repair (docs/design/efi-resident-stage.md). It is
 * not an AML interpreter: it executes nothing and resolves no reference. It follows the encodings of the ACPI
 * Specification 6.5 chapter 20 that open a named scope (Scope, Device, Processor, ThermalZone, PowerResource) and
 * the two that define a name (Name, Method), and it skips the data objects whose bytes could look like a name
 * (Buffer, String, the integer prefixes). Each power management name that it finds is given to the scope that
 * encloses its definition.
 */
#ifndef EFL_AML_H
#define EFL_AML_H
#include <stddef.h>

/* The processor power management names of ACPI 6.5 chapter 8 that the report looks for. One bit each. */
enum efl_aml_power_name {
	EFL_AML_CST = 0, EFL_AML_CSD, EFL_AML_PSS, EFL_AML_PCT, EFL_AML_PPC, EFL_AML_PSD, EFL_AML_CPC, EFL_AML_LPI,
	EFL_AML_POWER_NAMES
};
const char *efl_aml_power_name_text(enum efl_aml_power_name name);

/* What an object of the list is. */
enum efl_aml_kind {
	EFL_AML_PROCESSOR = 0,	/* a Processor object (deprecated in ACPI 6.5, still in many tables) */
	EFL_AML_DEVICE_CPU,	/* a Device with _HID "ACPI0007", the processor device */
	EFL_AML_DEVICE,		/* another Device that defines a power management name */
	EFL_AML_SCOPE		/* a Scope statement that defines a power management name */
};
const char *efl_aml_kind_text(enum efl_aml_kind kind);

#define EFL_AML_PATH_CHARS	64u	/* "\_PR_.C000" form: the name segments with their trailing underscores */
#define EFL_AML_MAX_OBJECTS	64u
#define EFL_AML_MAX_DEPTH	16u
#define EFL_AML_MAX_SEGMENTS	8u
#define EFL_AML_MAX_WATCH	8u

struct efl_aml_object {
	char path[EFL_AML_PATH_CHARS];
	enum efl_aml_kind kind;
	unsigned names;		/* bit (1 << efl_aml_power_name) for each name defined directly in this scope */
	unsigned proc_id;	/* EFL_AML_PROCESSOR: the ProcID operand of the Processor object, else 0 */
};

/* The thermal and fan names that the report counts. The counts are occurrences of the name's bytes anywhere in the
 * table, definitions and references alike. */
enum efl_aml_count {
	EFL_AML_TZ = 0, EFL_AML_TMP, EFL_AML_CRT, EFL_AML_PSV, EFL_AML_AC0, EFL_AML_FST, EFL_AML_FAN_STRING, EFL_AML_FAN_EISAID,
	EFL_AML_COUNTS
};
const char *efl_aml_count_text(enum efl_aml_count count);

struct efl_aml_scan {
	unsigned objects;
	unsigned dropped;		/* objects that did not fit in the list */
	unsigned processor_objects;	/* every Processor object, also the ones that did not fit */
	unsigned thermal_zones;		/* ThermalZone objects */
	unsigned malformed;		/* scope openings whose length or name did not parse; the scan went on */
	unsigned counts[EFL_AML_COUNTS];
	struct efl_aml_object object[EFL_AML_MAX_OBJECTS];
	/* The caller's watch list: watched[i] counts the objects that this table defines whose last name segment is
	 * watch[i] (a Name, a Method, a Device, a Processor, a ThermalZone or a PowerResource, in any scope). The
	 * caller sets watch and watch_count before the scan when it uses efl_aml_scan_watch. */
	unsigned watched[EFL_AML_MAX_WATCH];
};

/* Scans one table (its whole bytes, the 36-byte header included). Returns 0 when the bytes are not a DSDT or SSDT.
 * The list holds the processor objects, the processor devices, and every other scope that defines one of the power
 * management names, in the order of the table. */
int efl_aml_scan(const unsigned char *table, size_t bytes, struct efl_aml_scan *out);

/* The same scan with a watch list of name segments, 4 characters each. out->watched[i] is the count for watch[i]. */
int efl_aml_scan_watch(const unsigned char *table, size_t bytes, const char *const *watch, unsigned watch_count,
		       struct efl_aml_scan *out);

/* A PkgLength at p (ACPI 6.5 section 20.2.4). Returns its bytes and sets *length, 0 when it does not parse inside
 * end. */
size_t efl_aml_pkg_length(const unsigned char *p, const unsigned char *end, size_t *length);

/* An absolute NameString at p (ACPI 6.5 section 20.2.2) as text in the "\_PR_.C000" form. Returns its bytes, 0 when
 * it does not parse inside end, is not absolute, or does not fit in EFL_AML_PATH_CHARS. */
size_t efl_aml_absolute_path(const unsigned char *p, const unsigned char *end, char *path);

/* The 32-bit EISA ID of a seven-character ID such as "PNP0C0B", as the ASL macro EISAID() encodes it (ACPI 6.5
 * section 19.6.35). 0 when the text is not of that form. */
unsigned efl_aml_eisaid(const char *id);

#endif
