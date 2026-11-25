/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The write test of one sector. The contract is in flash_writetest.h. */
#include <string.h>
#include "flash_writetest.h"

static const char *const step_text[FWT_STEPS] = {
	"open", "read", "erase", "read", "write", "read", "erase-write", "read", "erase", "read", "close", "read"
};

const char *flash_write_test_step_text(enum flash_write_test_step step)
{
	return (unsigned)step < FWT_STEPS ? step_text[step] : "";
}

void flash_write_test_pattern(enum flash_write_test_step step, unsigned char *out)
{
	unsigned i;
	/* A counter, four constants, then a sequence without a short period. */
	for (i = 0; i < FLASH_SECTOR_BYTES; i++) {
		unsigned char b;
		if (i < 256u) b = (unsigned char)i;
		else if (i < 512u) b = i < 320u ? 0x00u : i < 384u ? 0xFFu : i < 448u ? 0x55u : 0xAAu;
		else b = (unsigned char)((i * 2654435761u + 0x5Au) >> 24);
		/* The erase-write step writes the other pattern: each bit that the write step left is the other way. */
		out[i] = step == FWT_ERASE_WRITE ? (unsigned char)~b : b;
	}
}

/* 1 when a read step found what the call before it must have left. */
static int read_as_expected(enum flash_write_test_step step, const unsigned char *block)
{
	unsigned char want[FLASH_SECTOR_BYTES];
	if (step == FWT_READ_WRITTEN || step == FWT_READ_ERASED_WRITTEN) {
		flash_write_test_pattern(step == FWT_READ_WRITTEN ? FWT_WRITE : FWT_ERASE_WRITE, want);
		return memcmp(block, want, FLASH_SECTOR_BYTES) == 0;
	}
	return flash_block_is_erased(block, FLASH_SECTOR_BYTES);
}

/* The step that comes after step. After a step that was not as expected, only the steps that put the sector back
 * and end the session follow. */
static enum flash_write_test_step next_step(enum flash_write_test_step step, int as_expected)
{
	if (!as_expected) {
		/* A failed open leaves no session to end. */
		if (step == FWT_OPEN) return FWT_STEPS;
		/* Up to the first write nothing is programmed: end the session. */
		if (step < FWT_WRITE) return FWT_CLOSE;
		/* From the first write on, the sector can hold a pattern: erase it again. */
		if (step < FWT_ERASE_LAST) return FWT_ERASE_LAST;
	}
	return (enum flash_write_test_step)(step + 1);
}

int flash_run_write_test(const struct flash_write_test_ops *ops, unsigned long sector,
			 struct flash_write_test_report *report)
{
	unsigned char block[FLASH_SECTOR_BYTES];
	enum flash_write_test_step step = FWT_OPEN;

	memset(report, 0, sizeof(*report));
	report->failed = FWT_STEPS;
	/* A table without each function runs nothing: no step is called through a null pointer. */
	if (ops == NULL || ops->open == NULL || ops->close == NULL || ops->read == NULL || ops->erase == NULL ||
	    ops->write == NULL || ops->erase_write == NULL)
		return 0;
	while (step < FWT_STEPS) {
		unsigned long long status;
		int as_expected;
		report->ran[step] = 1;
		switch (step) {
		case FWT_OPEN:
			status = ops->open(ops->context);
			break;
		case FWT_CLOSE:
			status = ops->close(ops->context);
			break;
		case FWT_ERASE:
		case FWT_ERASE_LAST:
			status = ops->erase(ops->context, sector);
			break;
		case FWT_WRITE:
		case FWT_ERASE_WRITE:
			flash_write_test_pattern(step, block);
			status = step == FWT_WRITE ? ops->write(ops->context, sector, block) :
						     ops->erase_write(ops->context, sector, block);
			break;
		default:
			memset(block, 0, sizeof(block));
			status = ops->read(ops->context, sector, block);
			break;
		}
		report->status[step] = status;
		as_expected = status == 0;
		switch (step) {
		case FWT_READ_FIRST:
		case FWT_READ_ERASED:
		case FWT_READ_WRITTEN:
		case FWT_READ_ERASED_WRITTEN:
		case FWT_READ_FINAL:
		case FWT_READ_OUTSIDE:
			if (as_expected) as_expected = read_as_expected(step, block);
			break;
		default:
			break;
		}
		report->as_expected[step] = as_expected != 0;
		if (!as_expected && report->failed == FWT_STEPS) report->failed = (unsigned char)step;
		step = next_step(step, as_expected);
	}
	return report->failed == FWT_STEPS;
}
