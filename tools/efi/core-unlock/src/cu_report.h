/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The read-only report (docs/design/efi-core-unlock.md, "The report"): the text of what the firmware shows before
 * Windows starts. This module only formats. The UEFI binding (report_uefi.c) collects the input: the MP Services
 * counts, one register sample per processor, the GOP modes. The ACPI part is read here through the parsers of
 * efl_acpi.c and efl_aml.c, over the caller's map function, so the host tests give it stored tables.
 */
#ifndef CU_REPORT_H
#define CU_REPORT_H
#include <stddef.h>
#include "efl_acpi.h"
#include "efl_aml.h"
#include "cu_msr.h"

#define CU_REPORT_NAME		"core-unlock-report.txt"
#define CU_REPORT_MAX_CPUS	64u
#define CU_REPORT_MAX_GOPS	4u
#define CU_REPORT_MAX_MODES	48u
#define CU_REPORT_BYTES		(96u * 1024u)

/* One processor of MP Services. The status values are EFI_STATUS values, printed as they are. */
struct cu_report_cpu {
	unsigned number;		/* the MP Services processor number */
	int info_ok;			/* GetProcessorInfo answered */
	unsigned long long info_status;
	unsigned long long processor_id;	/* the APIC ID that MP Services gives */
	unsigned status_flag;		/* bit 0 BSP, bit 1 enabled, bit 2 healthy (PI volume 2, MP Services) */
	unsigned package, core, thread;
	int sampled;			/* the register sample ran on this processor */
	unsigned long long sample_status;	/* the StartupThisAP status, or 0 for the BSP */
	struct cu_cpu_sample sample;
};

struct cu_report_mode {
	unsigned number;
	int ok;				/* QueryMode answered */
	unsigned width, height, pixel_format, pixels_per_line;
	unsigned mask[4];		/* red, green, blue, reserved when pixel_format is PixelBitMask */
};

struct cu_report_gop {
	unsigned max_mode, current;
	unsigned modes, dropped;
	struct cu_report_mode mode[CU_REPORT_MAX_MODES];
};

struct cu_report_input {
	const char *version;
	int mp_present;
	unsigned long long mp_status;	/* LocateProtocol or GetNumberOfProcessors, when either failed */
	unsigned mp_all, mp_enabled;
	unsigned cpus, cpus_dropped;
	struct cu_report_cpu cpu[CU_REPORT_MAX_CPUS];
	int bsp_only_sampled;		/* no MP Services: the BSP's sample is cpu[0] */
	const struct efl_acpi *acpi;
	unsigned gops, gops_dropped;
	struct cu_report_gop gop[CU_REPORT_MAX_GOPS];
};

/* The report as text, one line per '\n'. Returns the characters written; *truncated is set when the text did not
 * fit in capacity. */
size_t cu_report_write(const struct cu_report_input *in, char *out, size_t capacity, int *truncated);

/* The power management names over every DSDT and SSDT of the ACPI tables: how many listed objects define each. */
struct cu_report_power_summary {
	unsigned tables;		/* DSDT and SSDT tables that were scanned */
	unsigned objects;		/* listed objects in them */
	unsigned processor_objects;
	unsigned with_name[EFL_AML_POWER_NAMES];
};
void cu_report_power_summary(const struct efl_acpi *acpi, struct cu_report_power_summary *out);

#endif
