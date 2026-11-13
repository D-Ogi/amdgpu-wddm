/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The load options of the core unlock, read from the options of the stage (stage_options.h), and the name of its
 * marker file. Pure code, tested on the host.
 */
#ifndef CU_OPTIONS_H
#define CU_OPTIONS_H
#include "stage_options.h"

/* The reset after the message (docs/design/efi-core-unlock.md, "The reset"). Which one keeps the core mask is not
 * measured. */
enum cu_reset_method {
	CU_RESET_WARM = 0,	/* the runtime service ResetSystem with EfiResetWarm: the default */
	CU_RESET_FADT = 1,	/* the FADT reset register with the FADT reset value */
	CU_RESET_NONE = 2,	/* no reset: the message goes out and Windows starts. The marker is not written */
	CU_RESET_COUNT
};

struct cu_options {
	enum cu_reset_method reset;
	int report;		/* report=1: write the read-only report before the decision. Default 0 */
	int key;		/* key=0: no key window before the unlock. Default 1 */
};

/* "reset=warm", "reset=fadt", "reset=none", "report=0" or "report=1", "key=0" or "key=1". A value that is not one
 * of these keeps the default and stays unclaimed, so the core reports it. */
void cu_options_read(struct stage_options *options, struct cu_options *out);
const char *cu_reset_method_name(enum cu_reset_method method);

/* The marker beside the stage image. */
#define CU_MARKER_NAME "core-unlock.mark"

#endif
