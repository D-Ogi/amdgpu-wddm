/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The write test of two free sectors (docs/design/p6-flash.md, the measurements on unit A). Pure code.
 *
 * The test is the first write of a unit through the flash protocol. S is the sector that the load option names, and
 * N is the next sector. The test calls each function of the protocol, and after each call that can change a sector
 * it reads and compares the bytes:
 *
 *   1  open                         12 read S: erased
 *   2  read S: erased               13 read N: the third pattern (the erase of S left N)
 *   3  erase S                      14 write 4 bytes into the erased S
 *   4  read S: erased               15 read S: erased, but for the 4 bytes
 *   5  write S: the first pattern   16 erase S
 *   6  read S: the first pattern    17 read S: erased
 *   7  erase-write S: the second    18 erase N
 *   8  read S: the second pattern   19 read N: erased
 *   9  write N: the third pattern   20 close
 *   10 read N: the third pattern    21 read S: erased, outside the session
 *   11 erase S                      22 read N: erased, outside the session
 *
 * Step 13 shows that an erase of one sector leaves the next sector. Step 14 is the write of the signature of a
 * volume header after the write of its first sector, as the job apply needs it. The two last erases put both sectors
 * back to the state that the image had.
 *
 * The calls go through struct flash_write_test_ops. The binding of the stage fills it with the functions of the
 * protocol of the firmware; the host tests fill it with a model of a flash chip.
 */
#ifndef FLASH_WRITETEST_H
#define FLASH_WRITETEST_H
#include <stddef.h>
#include "flash.h"

#define FLASH_TEST_HEADER_OFFSET	0x28u	/* where the signature of a volume header stands in its first sector */
#define FLASH_TEST_HEADER_BYTES		4u

enum flash_write_test_step {
	FWT_OPEN = 0,
	FWT_READ_S0,
	FWT_ERASE_S,
	FWT_READ_S1,
	FWT_WRITE_S,
	FWT_READ_S2,
	FWT_ERASE_WRITE_S,
	FWT_READ_S3,
	FWT_WRITE_N,
	FWT_READ_N0,
	FWT_ERASE_S_LAST,
	FWT_READ_S4,
	FWT_READ_N1,
	FWT_WRITE_HEADER,
	FWT_READ_S5,
	FWT_ERASE_S_FINAL,
	FWT_READ_S6,
	FWT_ERASE_N,
	FWT_READ_N2,
	FWT_CLOSE,
	FWT_READ_S7,
	FWT_READ_N3,
	FWT_STEPS
};

/* The functions take image offsets, not the addresses in the ROM window. Each gives 0 for success and else the
 * status of the protocol. erase and erase_write take one sector; read and write take a count of bytes. */
struct flash_write_test_ops {
	void *context;
	unsigned long long (*open)(void *context);
	unsigned long long (*close)(void *context);
	unsigned long long (*read)(void *context, unsigned long offset, unsigned char *block, size_t bytes);
	unsigned long long (*erase)(void *context, unsigned long offset);
	unsigned long long (*write)(void *context, unsigned long offset, const unsigned char *block, size_t bytes);
	unsigned long long (*erase_write)(void *context, unsigned long offset, const unsigned char *block);
};

struct flash_write_test_report {
	unsigned char failed;			/* the first step that was not as expected; FWT_STEPS when each was */
	unsigned char ran[FWT_STEPS];		/* 1 when the call of the step was made */
	unsigned long long status[FWT_STEPS];	/* the status of each call that ran */
	signed char as_expected[FWT_STEPS];	/* 1 when the call succeeded and left what it must; else 0 */
};

/* The name of a step, for the console and the result file. */
const char *flash_write_test_step_text(enum flash_write_test_step step);

/* The bytes that a write step writes: FLASH_SECTOR_BYTES, or FLASH_TEST_HEADER_BYTES for the step of the header. */
void flash_write_test_pattern(enum flash_write_test_step step, unsigned char *out);

/* 1 when the bytes are what the read step must show. block has FLASH_SECTOR_BYTES bytes. */
int flash_write_test_expected(enum flash_write_test_step step, const unsigned char *block);

/* Runs the test on the sector and the next sector. The caller made sure that both are free (flash_sector_place).
 * When a step is not as expected, the test goes on only with the steps that erase both sectors again and end the
 * session. A sector that the test programmed is erased at the end, unless that erase fails too. 1 when each step
 * was as expected. */
int flash_run_write_test(const struct flash_write_test_ops *ops, unsigned long sector,
			 struct flash_write_test_report *report);

#endif
