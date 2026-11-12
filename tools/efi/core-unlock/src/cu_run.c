/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The steps of one start (cu_run.h). */
#include <stddef.h>
#include "cu_run.h"

static const char *const why_text[CU_WHY_COUNT] = {
	"not a BC-250 (PCI function and SMBIOS identity): nothing to do",
	"the processor count is not 12 or 16: nothing to do",
	"16 logical processors: all 8 cores run, nothing to do",
	"16 logical processors after an attempt: the cores run, the marker goes",
	"the marker file cannot be read: nothing to do",
	"still 12 logical processors after the attempt: the reset did not keep the core mask",
	"12 logical processors and an earlier attempt failed: nothing to do (delete the marker to try again)",
	"key pressed: skipped",
	"12 logical processors: the two other cores are off, unlock",
};

enum cu_outcome cu_run(const struct cu_ops *ops, int resets)
{
	struct cu_inputs in;
	struct cu_decision d;

	in.board_ok = ops->board(ops->context) ? 1 : 0;
	in.processors = in.board_ok ? ops->processors(ops->context) : 0u;
	in.marker = in.board_ok ? ops->marker_read(ops->context) : CU_MARKER_NONE;
	in.key_pressed = 0;
	d = cu_decide(&in);
	/* The window costs a second or two, so only a start that would act opens it. */
	if (d.action == CU_ACT_UNLOCK && ops->key_window(ops->context)) {
		in.key_pressed = 1;
		d = cu_decide(&in);
	}
	ops->say(ops->context, why_text[d.reason]);

	switch (d.action) {
	case CU_ACT_CLEAR:
		if (ops->marker_delete(ops->context)) ops->say(ops->context, "the marker cannot be deleted");
		return CU_OUT_CLEARED;
	case CU_ACT_MARK_FAILED:
		/* The failed mark stops every further attempt, which a "delete then write" makes safe either way: a
		 * write that fails leaves no marker, and the next start then tries once more with one reset. */
		if (ops->marker_delete(ops->context) || ops->marker_write(ops->context, CU_MARKER_FAILED))
			ops->say(ops->context, "the failed mark cannot be written: the next start tries again");
		return CU_OUT_MARKED_FAILED;
	case CU_ACT_UNLOCK:
		break;
	case CU_ACT_BOOT:
	default:
		return CU_OUT_NOTHING;
	}

	/* The loop guard goes to the disk before anything goes to the firmware. Without it, no reset. */
	if (resets && ops->marker_write(ops->context, CU_MARKER_ATTEMPT)) {
		ops->say(ops->context, "the marker cannot be written: no message, no reset");
		return CU_OUT_FAILED;
	}
	if (ops->unlock(ops->context)) {
		/* Nothing was reset, so the attempt did not happen: the marker goes and the next start tries again. */
		if (resets && ops->marker_delete(ops->context))
			ops->say(ops->context, "the marker cannot be deleted: the next start reports a failed attempt");
		ops->say(ops->context, "the message failed: no reset");
		return CU_OUT_FAILED;
	}
	if (!resets) {
		ops->say(ops->context, "message accepted, no reset asked for: the cores come at the next warm restart");
		return CU_OUT_SENT;
	}
	ops->say(ops->context, "message accepted: reset now");
	(void)ops->reset(ops->context);
	/* Still here: the reset did not happen. The marker goes, so the next start does not count this as a failed
	 * attempt, and the message stays in the chip for the next warm restart of Windows. */
	if (ops->marker_delete(ops->context))
		ops->say(ops->context, "the marker cannot be deleted: the next start reports a failed attempt");
	ops->say(ops->context, "the reset did not happen: the cores come at the next warm restart");
	return CU_OUT_FAILED;
}
