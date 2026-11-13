/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The load options of the core unlock (cu_options.h). */
#include "cu_options.h"

void cu_options_read(struct stage_options *options, struct cu_options *out)
{
	static const char *const methods[CU_RESET_COUNT] = { "warm", "fadt", "none" };
	out->reset = (enum cu_reset_method)stage_option_choice(options, "reset", methods, CU_RESET_COUNT, CU_RESET_WARM);
	out->report = stage_option_flag(options, "report", 0);
	out->key = stage_option_flag(options, "key", 1);
}

const char *cu_reset_method_name(enum cu_reset_method method)
{
	switch (method) {
	case CU_RESET_WARM: return "warm (ResetSystem EfiResetWarm)";
	case CU_RESET_FADT: return "fadt (FADT reset register)";
	case CU_RESET_NONE: return "none";
	default: return "unknown";
	}
}
