/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The write test of one sector (docs/design/p6-flash.md, the measurements on unit A). Pure code.
 *
 * The test is the first write of a unit through the flash protocol. It calls each function of the protocol on one
 * free sector: open, read, erase, read, write, read, erase-write, read, erase, read, close, read. After each call
 * that can change the sector, the test reads the sector and compares it with what the call must have left. The last
 * erase puts the sector back to the state that the image had.
 *
 * The calls go through struct flash_write_test_ops. The binding of the stage fills it with the functions of the
 * protocol of the firmware; the host tests fill it with a model of a flash chip.
 */
#ifndef FLASH_WRITETEST_H
#define FLASH_WRITETEST_H
#include "flash.h"

enum flash_write_test_step {
	FWT_OPEN = 0,
	FWT_READ_FIRST,		/* before the first erase: the sector must be erased */
	FWT_ERASE,
	FWT_READ_ERASED,
	FWT_WRITE,
	FWT_READ_WRITTEN,
	FWT_ERASE_WRITE,
	FWT_READ_ERASED_WRITTEN,
	FWT_ERASE_LAST,
	FWT_READ_FINAL,
	FWT_CLOSE,
	FWT_READ_OUTSIDE,	/* after the close: the sector must be erased again */
	FWT_STEPS
};

/* The functions take the image offset of the sector, not its address in the ROM window. Each gives 0 for success
 * and else the status of the protocol. read fills FLASH_SECTOR_BYTES bytes; write and erase_write read them. */
struct flash_write_test_ops {
	void *context;
	unsigned long long (*open)(void *context);
	unsigned long long (*close)(void *context);
	unsigned long long (*read)(void *context, unsigned long sector, unsigned char *block);
	unsigned long long (*erase)(void *context, unsigned long sector);
	unsigned long long (*write)(void *context, unsigned long sector, const unsigned char *block);
	unsigned long long (*erase_write)(void *context, unsigned long sector, const unsigned char *block);
};

struct flash_write_test_report {
	unsigned char failed;			/* the first step that was not as expected; FWT_STEPS when each was */
	unsigned char ran[FWT_STEPS];		/* 1 when the call of the step was made */
	unsigned long long status[FWT_STEPS];	/* the status of each call that ran */
	signed char as_expected[FWT_STEPS];	/* 1 when the call succeeded and left what it must; else 0 */
};

/* The name of a step, for the console and the result file. */
const char *flash_write_test_step_text(enum flash_write_test_step step);

/* The bytes that the test writes in a step: FLASH_SECTOR_BYTES. The two write steps write different patterns. */
void flash_write_test_pattern(enum flash_write_test_step step, unsigned char *out);

/* Runs the test on one sector. The caller made sure that the sector is free (flash_sector_place). When a step is
 * not as expected, the test goes on only with the steps that erase the sector again and end the session. A sector
 * that the test programmed is erased at the end, unless that erase fails too. 1 when each step was as expected. */
int flash_run_write_test(const struct flash_write_test_ops *ops, unsigned long sector,
			 struct flash_write_test_report *report);

#endif
