/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The UEFI binding of the read-only report (cu_report.h): it collects the input from the firmware, writes the text
 * to the report file beside the image and prints it on the console. It reads only. The one write is the report
 * file; a failure of that write is a console line and nothing more.
 */
#ifndef REPORT_UEFI_H
#define REPORT_UEFI_H
#include "efi.h"
#include "efl_acpi.h"

typedef void (*report_say_fn)(void *context, const char *line);

struct report_uefi {
	EFI_SYSTEM_TABLE *st;
	EFI_FILE_PROTOCOL *root;		/* the volume of the image, or NULL */
	const unsigned short *path;		/* the report file on that volume, or NULL */
	const struct efl_acpi *acpi;
	const char *version;
	report_say_fn say;
	void *say_context;
};

void report_uefi_run(const struct report_uefi *r);

#endif
