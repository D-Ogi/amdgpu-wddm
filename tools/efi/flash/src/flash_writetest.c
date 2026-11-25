/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The write test of two free sectors. The contract is in flash_writetest.h. */
#include <string.h>
#include "flash_writetest.h"

/* S is the sector that the load option names; N is the next sector. */
static const char *const step_text[FWT_STEPS] = {
	"open",
	"read S", "erase S", "read S", "write S", "read S", "erase-write S", "read S",
	"write N", "read N",
	"erase S", "read S", "read N", "write S header", "read S", "erase S", "read S",
	"erase N", "read N",
	"close", "read S", "read N"
};

const char *flash_write_test_step_text(enum flash_write_test_step step)
{
	return (unsigned)step < FWT_STEPS ? step_text[step] : "";
}

void flash_write_test_pattern(enum flash_write_test_step step, unsigned char *out)
{
	static const unsigned char signature[FLASH_TEST_HEADER_BYTES] = { '_', 'F', 'V', 'H' };
	unsigned i;
	if (step == FWT_WRITE_HEADER) {
		memcpy(out, signature, sizeof(signature));
		return;
	}
	/* A counter, four constants, then a sequence without a short period. */
	for (i = 0; i < FLASH_SECTOR_BYTES; i++) {
		unsigned char b;
		if (i < 256u) b = (unsigned char)i;
		else if (i < 512u) b = i < 320u ? 0x00u : i < 384u ? 0xFFu : i < 448u ? 0x55u : 0xAAu;
		else b = (unsigned char)((i * 2654435761u + 0x5Au) >> 24);
		/* The erase-write step writes the complement of the first pattern. The next sector gets the first pattern
		 * with the two halves of each byte exchanged. */
		if (step == FWT_ERASE_WRITE_S) b = (unsigned char)~b;
		else if (step == FWT_WRITE_N) b = (unsigned char)((b << 4) | (b >> 4));
		out[i] = b;
	}
}

int flash_write_test_expected(enum flash_write_test_step step, const unsigned char *block)
{
	unsigned char want[FLASH_SECTOR_BYTES];
	switch (step) {
	case FWT_READ_S2:
		flash_write_test_pattern(FWT_WRITE_S, want);
		break;
	case FWT_READ_S3:
		flash_write_test_pattern(FWT_ERASE_WRITE_S, want);
		break;
	case FWT_READ_N0:
	case FWT_READ_N1:
		flash_write_test_pattern(FWT_WRITE_N, want);
		break;
	case FWT_READ_S5:
		/* The erased sector with the signature of a volume header at its place. */
		memset(want, 0xFF, sizeof(want));
		flash_write_test_pattern(FWT_WRITE_HEADER, want + FLASH_TEST_HEADER_OFFSET);
		break;
	default:
		/* Each other read step: the sector is erased. */
		return flash_block_is_erased(block, FLASH_SECTOR_BYTES);
	}
	return memcmp(block, want, FLASH_SECTOR_BYTES) == 0;
}

static int is_read(enum flash_write_test_step step)
{
	switch (step) {
	case FWT_READ_S0:
	case FWT_READ_S1:
	case FWT_READ_S2:
	case FWT_READ_S3:
	case FWT_READ_N0:
	case FWT_READ_S4:
	case FWT_READ_N1:
	case FWT_READ_S5:
	case FWT_READ_S6:
	case FWT_READ_N2:
	case FWT_READ_S7:
	case FWT_READ_N3:
		return 1;
	default:
		return 0;
	}
}

static int reads_n(enum flash_write_test_step step)
{
	return step == FWT_READ_N0 || step == FWT_READ_N1 || step == FWT_READ_N2 || step == FWT_READ_N3;
}

/* The step that comes after step. After a step that was not as expected, only the steps that put both sectors back
 * and end the session follow. */
static enum flash_write_test_step next_step(enum flash_write_test_step step, int as_expected)
{
	if (!as_expected) {
		/* A failed open leaves no session to end. */
		if (step == FWT_OPEN) return FWT_STEPS;
		/* Up to the first write nothing is programmed: end the session. */
		if (step < FWT_WRITE_S) return FWT_CLOSE;
		/* From the first write on, a sector can hold a pattern: erase S again, then N. */
		if (step < FWT_ERASE_S_FINAL) return FWT_ERASE_S_FINAL;
	}
	return (enum flash_write_test_step)(step + 1);
}

int flash_run_write_test(const struct flash_write_test_ops *ops, unsigned long sector,
			 struct flash_write_test_report *report)
{
	unsigned char block[FLASH_SECTOR_BYTES], small[FLASH_TEST_HEADER_BYTES];
	unsigned long next = sector + FLASH_SECTOR_BYTES;
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
		case FWT_ERASE_S:
		case FWT_ERASE_S_LAST:
		case FWT_ERASE_S_FINAL:
			status = ops->erase(ops->context, sector);
			break;
		case FWT_ERASE_N:
			status = ops->erase(ops->context, next);
			break;
		case FWT_ERASE_WRITE_S:
			flash_write_test_pattern(step, block);
			status = ops->erase_write(ops->context, sector, block);
			break;
		case FWT_WRITE_S:
		case FWT_WRITE_N:
			flash_write_test_pattern(step, block);
			status = ops->write(ops->context, step == FWT_WRITE_S ? sector : next, block, FLASH_SECTOR_BYTES);
			break;
		case FWT_WRITE_HEADER:
			flash_write_test_pattern(step, small);
			status = ops->write(ops->context, sector + FLASH_TEST_HEADER_OFFSET, small, sizeof(small));
			break;
		default:
			/* A read step. The block is zero first: a call that fails leaves nothing of an earlier step in it. */
			memset(block, 0, sizeof(block));
			status = ops->read(ops->context, reads_n(step) ? next : sector, block, FLASH_SECTOR_BYTES);
			break;
		}
		report->status[step] = status;
		as_expected = status == 0 && (!is_read(step) || flash_write_test_expected(step, block));
		report->as_expected[step] = as_expected != 0;
		if (!as_expected && report->failed == FWT_STEPS) report->failed = (unsigned char)step;
		step = next_step(step, as_expected);
	}
	return report->failed == FWT_STEPS;
}
