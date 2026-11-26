/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The decision table of core-unlock (cu_decide.h). The processor count is read through the driver's own rule
 * (bc250_cpu_mask_in_force), so "12" and "16" are not numbers of this file. */
#include <string.h>
#include "cu_decide.h"
#include "bc250_cpu.h"

static struct cu_decision decision(enum cu_action action, enum cu_reason reason)
{
	struct cu_decision d;
	d.action = action;
	d.reason = reason;
	return d;
}

struct cu_decision cu_decide(const struct cu_inputs *in)
{
	unsigned mask;
	if (!in->board_ok) return decision(CU_ACT_BOOT, CU_WHY_NOT_BOARD);
	mask = bc250_cpu_mask_in_force(in->processors);
	if (mask == BC250_CPU_MASK_FULL) {
		/* The cores run. A marker of any state is now stale: removing it lets the next cold boot try again. A
		 * marker that cannot be read cannot be removed either, and that changes nothing at this start. */
		if (in->marker == CU_MARKER_NONE || in->marker == CU_MARKER_ERROR)
			return decision(CU_ACT_BOOT, CU_WHY_FULL);
		return decision(CU_ACT_CLEAR, CU_WHY_FULL_AFTER);
	}
	if (mask != BC250_CPU_MASK_STOCK) return decision(CU_ACT_BOOT, CU_WHY_COUNT_UNKNOWN);
	switch (in->marker) {
	case CU_MARKER_NONE:
		break;
	case CU_MARKER_ATTEMPT:
		/* The reset of the last start did not keep the mask. Never a second reset in a row. */
		return decision(CU_ACT_MARK_FAILED, CU_WHY_ATTEMPT_FAILED);
	case CU_MARKER_FAILED:
	case CU_MARKER_UNKNOWN:
		return decision(CU_ACT_BOOT, CU_WHY_EARLIER_FAILED);
	case CU_MARKER_ERROR:
	default:
		return decision(CU_ACT_BOOT, CU_WHY_MARKER_ERROR);
	}
	if (in->key_pressed) return decision(CU_ACT_BOOT, CU_WHY_KEY);
	return decision(CU_ACT_UNLOCK, CU_WHY_STOCK);
}

static const char marker_attempt[] = "bc250 core-unlock attempt\r\n";
static const char marker_failed[] = "bc250 core-unlock failed\r\n";

const char *cu_marker_text(enum cu_marker state)
{
	if (state == CU_MARKER_ATTEMPT) return marker_attempt;
	if (state == CU_MARKER_FAILED) return marker_failed;
	return NULL;
}

enum cu_marker cu_marker_parse(const unsigned char *data, unsigned long bytes)
{
	if (data == NULL) return CU_MARKER_UNKNOWN;
	if (bytes == sizeof(marker_attempt) - 1u && !memcmp(data, marker_attempt, bytes)) return CU_MARKER_ATTEMPT;
	if (bytes == sizeof(marker_failed) - 1u && !memcmp(data, marker_failed, bytes)) return CU_MARKER_FAILED;
	return CU_MARKER_UNKNOWN;
}
