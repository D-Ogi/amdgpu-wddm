/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the description of the firmware image (flash.h) and of the write test (flash_writetest.h): the three
 * digests, the firmware volumes, the LZMA streams, the place of a sector in the image, and the steps of the write test
 * on a model of a flash chip. The image tests run on an image that this test builds, with the file layout of the
 * stock image: each volume starts with a pad file for the extended header. The last part runs on the stock image of
 * the vendor, which is outside the repository, and says SKIP when it is not there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efl_test.h"
#include "efl_sha256.h"
#include "flash.h"
#include "flash_writetest.h"

#define FFS_HEADER_BYTES	24u
#define SECTION_GUIDED_BYTES	24u
#define LZMA_HEADER_BYTES	13u
#define STREAM_BYTES		1000u
#define STORE_FILE_BYTES	0x1FF88u
#define EXT_HEADER_PAD_BYTES	0x2Cu	/* the pad file at 0x48 of each volume; the next file is at 0x78 */
#define FIRST_FILE		0x78u
#define PAD_AT			0x4A0u	/* where the last pad file of the outer volume starts: its image file ends at 0x49D */
#define PAD_BYTES		32u
/* The image file of the outer volume: its header, the section header, the stream header and the stream. */
#define OUTER_FILE_BYTES	(FFS_HEADER_BYTES + SECTION_GUIDED_BYTES + LZMA_HEADER_BYTES + STREAM_BYTES)
static const unsigned char lzma_guid[16] = {	/* EE4E5898-3914-4259-9D6E-DC7BD79403CF */
	0x98, 0x58, 0x4E, 0xEE, 0x14, 0x39, 0x59, 0x42, 0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF
};

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
	put16(p, (unsigned)v);
	put16(p + 2, (unsigned)(v >> 16));
}

static void put64(unsigned char *p, unsigned long long v)
{
	put32(p, (unsigned long)v);
	put32(p + 4, (unsigned long)(v >> 32));
}

/* A firmware volume header at the offset: one block map entry, HeaderLength 0x48. */
static void put_volume(unsigned char *image, unsigned long offset, unsigned long long bytes, unsigned attributes)
{
	memset(image + offset, 0, 0x48);
	put64(image + offset + 0x20, bytes);
	memcpy(image + offset + 0x28, "_FVH", 4);
	put32(image + offset + 0x2C, attributes);
	put16(image + offset + 0x30, 0x48);
	image[offset + 0x37] = 2;
	put32(image + offset + 0x38, 1);
	put32(image + offset + 0x3C, (unsigned long)bytes);
}

/* A file header at the offset, with no body: the image is erased there. Returns the offset of the next file, on an
 * 8-byte boundary. */
static unsigned long put_file(unsigned char *image, unsigned long at, unsigned type, unsigned long bytes, unsigned char state)
{
	memset(image + at, 0, FFS_HEADER_BYTES);
	image[at + 16] = 0x5A;		/* the name is not all FF: the file is not erased space */
	image[at + 18] = (unsigned char)type;
	image[at + 20] = (unsigned char)bytes;
	image[at + 21] = (unsigned char)(bytes >> 8);
	image[at + 22] = (unsigned char)(bytes >> 16);
	image[at + 23] = state;
	return (at + bytes + 7u) & ~7ul;
}

/* An image of 16 MiB with the attributes of the stock image: the store volume has erase polarity 1, and its raw file
 * is marked for update. */
static void build_image(unsigned char *image)
{
	unsigned long at, section;
	memset(image, 0xFF, FLASH_IMAGE_BYTES);
	put_volume(image, FLASH_STORE_OFFSET, FLASH_STORE_BYTES, 0x4FEFF);
	put_file(image, 0x48, 0xF0, EXT_HEADER_PAD_BYTES, (unsigned char)~0x07u);
	put_file(image, FIRST_FILE, 0x01, STORE_FILE_BYTES, (unsigned char)~0x0Fu);
	put_volume(image, FLASH_OUTER_OFFSET, 0x320000, 0x4FEFF);
	put_file(image, FLASH_OUTER_OFFSET + 0x48, 0xF0, EXT_HEADER_PAD_BYTES, (unsigned char)~0x07u);
	at = put_file(image, FLASH_OUTER_OFFSET + FIRST_FILE, 0x0B, OUTER_FILE_BYTES, (unsigned char)~0x07u);
	if (at - FLASH_OUTER_OFFSET != PAD_AT) {
		printf("FAIL the test image: the image file of the outer volume ends at 0x%lX\n", at);
		failures++;
	}
	section = FLASH_OUTER_OFFSET + FIRST_FILE + FFS_HEADER_BYTES;
	image[section] = (unsigned char)(SECTION_GUIDED_BYTES + LZMA_HEADER_BYTES + STREAM_BYTES);
	image[section + 1] = (unsigned char)((SECTION_GUIDED_BYTES + LZMA_HEADER_BYTES + STREAM_BYTES) >> 8);
	image[section + 3] = 0x02;
	memcpy(image + section + 4, lzma_guid, 16);
	put16(image + section + 20, SECTION_GUIDED_BYTES);
	image[section + SECTION_GUIDED_BYTES] = 0x5D;
	put64(image + section + SECTION_GUIDED_BYTES + 5, 0x418000);
	put_file(image, at, 0xF0, PAD_BYTES, (unsigned char)~0x07u);
	put_volume(image, FLASH_PEI_OFFSET, 0x200000, 0xCFEFF);
	put_file(image, FLASH_PEI_OFFSET + 0x48, 0xF0, EXT_HEADER_PAD_BYTES, (unsigned char)~0x07u);
	at = put_file(image, FLASH_PEI_OFFSET + FIRST_FILE, 0x06, 0x400, (unsigned char)~0x07u);
	put_file(image, at, 0x06, 0x1000, (unsigned char)~0x07u);
}

static int same(const unsigned char a[FLASH_DIGEST_BYTES], const unsigned char b[FLASH_DIGEST_BYTES])
{
	return !memcmp(a, b, FLASH_DIGEST_BYTES);
}

/* The digest of the first bytes of the image with the variable store read as zeros, from the library itself. */
static void expected_base(const unsigned char *image, size_t bytes, unsigned char digest[FLASH_DIGEST_BYTES])
{
	unsigned char *copy = malloc(bytes ? bytes : 1u);
	size_t store;
	if (copy == NULL) {
		memset(digest, 0, FLASH_DIGEST_BYTES);
		return;
	}
	memcpy(copy, image, bytes);
	store = bytes < FLASH_STORE_BYTES ? bytes : FLASH_STORE_BYTES;
	memset(copy, 0, store);
	efl_sha256(copy, bytes, digest);
	free(copy);
}

/* ---- the write test on a model of a flash chip ------------------------------------------------------------------ */

/* The chip of one sector. An erase sets 0xFF, and a write can only clear bits. One call can be made to answer an
 * error. That call changes nothing. */
struct chip {
	unsigned char bytes[FLASH_SECTOR_BYTES];
	unsigned calls;			/* the calls so far, from 1 */
	unsigned fail_call;		/* this call answers an error; 0: none */
	int no_erase, no_write;		/* the erase and the write change nothing, and answer success */
};
#define TEST_SECTOR		0xC24000ul
#define CHIP_ERROR		0x800000000000000Eull
#define BIT(step)		(1u << (step))
#define RAN_ALL			((1u << FWT_STEPS) - 1u)
#define RAN_SESSION		(BIT(FWT_OPEN) | BIT(FWT_READ_FIRST) | BIT(FWT_CLOSE) | BIT(FWT_READ_OUTSIDE))

static struct chip chip;

static unsigned long long chip_call(struct chip *c)
{
	c->calls++;
	return c->calls == c->fail_call ? CHIP_ERROR : 0;
}

static unsigned long long chip_open(void *context)
{
	return chip_call(context);
}

static unsigned long long chip_close(void *context)
{
	return chip_call(context);
}

static unsigned long long chip_read(void *context, unsigned long sector, unsigned char *block)
{
	struct chip *c = context;
	unsigned long long status = chip_call(c);
	CHECK(sector == TEST_SECTOR);
	memcpy(block, c->bytes, FLASH_SECTOR_BYTES);
	return status;
}

static unsigned long long chip_erase(void *context, unsigned long sector)
{
	struct chip *c = context;
	unsigned long long status = chip_call(c);
	CHECK(sector == TEST_SECTOR);
	if (status == 0 && !c->no_erase) memset(c->bytes, 0xFF, FLASH_SECTOR_BYTES);
	return status;
}

/* A program without an erase: a bit that is clear stays clear. */
static void chip_program(struct chip *c, const unsigned char *block)
{
	unsigned i;
	if (c->no_write) return;
	for (i = 0; i < FLASH_SECTOR_BYTES; i++) c->bytes[i] &= block[i];
}

static unsigned long long chip_write(void *context, unsigned long sector, const unsigned char *block)
{
	struct chip *c = context;
	unsigned long long status = chip_call(c);
	CHECK(sector == TEST_SECTOR);
	if (status == 0) chip_program(c, block);
	return status;
}

static unsigned long long chip_erase_write(void *context, unsigned long sector, const unsigned char *block)
{
	struct chip *c = context;
	unsigned long long status = chip_call(c);
	CHECK(sector == TEST_SECTOR);
	if (status == 0) {
		if (!c->no_erase) memset(c->bytes, 0xFF, FLASH_SECTOR_BYTES);
		chip_program(c, block);
	}
	return status;
}

static struct flash_write_test_ops ops = { NULL, chip_open, chip_close, chip_read, chip_erase, chip_write, chip_erase_write };

/* A new erased chip. fail_call is the call that answers an error, counted from 1; 0 when each call succeeds. */
static void chip_start(unsigned fail_call)
{
	memset(&chip, 0, sizeof(chip));
	memset(chip.bytes, 0xFF, FLASH_SECTOR_BYTES);
	chip.fail_call = fail_call;
}

/* The steps that ran, as one bit for each. */
static unsigned ran_bits(const struct flash_write_test_report *report)
{
	unsigned bits = 0, i;
	for (i = 0; i < FWT_STEPS; i++) bits |= (unsigned)report->ran[i] << i;
	return bits;
}

/* 1 when each step that ran answered success and was as expected. */
static int all_as_expected(const struct flash_write_test_report *report)
{
	unsigned i;
	for (i = 0; i < FWT_STEPS; i++)
		if (report->ran[i] && (report->status[i] != 0 || report->as_expected[i] != 1)) return 0;
	return 1;
}

static void test_write_test(void)
{
	static const struct flash_write_test_ops empty = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	struct flash_write_test_report report;
	unsigned char a[FLASH_SECTOR_BYTES], b[FLASH_SECTOR_BYTES];
	unsigned i, holds;

	ops.context = &chip;

	/* Each step has a name. The number after the last step has none. */
	holds = 1;
	for (i = 0; i < FWT_STEPS; i++) holds = holds && flash_write_test_step_text((enum flash_write_test_step)i)[0] != 0;
	CHECK(holds && flash_write_test_step_text(FWT_STEPS)[0] == 0);

	/* The two patterns: each byte of the second is the other way round, and a pattern is the same at each call. */
	flash_write_test_pattern(FWT_WRITE, a);
	flash_write_test_pattern(FWT_ERASE_WRITE, b);
	holds = 1;
	for (i = 0; i < FLASH_SECTOR_BYTES; i++) holds = holds && (a[i] ^ b[i]) == 0xFF;
	CHECK(holds);
	flash_write_test_pattern(FWT_ERASE_WRITE, a);
	CHECK(!memcmp(a, b, FLASH_SECTOR_BYTES));

	/* No table of functions: the test does not run. */
	chip_start(0);
	CHECK(!flash_run_write_test(&empty, TEST_SECTOR, &report) && !flash_run_write_test(NULL, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_STEPS && chip.calls == 0);

	/* A chip that answers as it must: each step runs, each is as expected, and the sector is erased at the end. */
	chip_start(0);
	CHECK(flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_STEPS && chip.calls == FWT_STEPS && ran_bits(&report) == RAN_ALL && all_as_expected(&report));
	CHECK(flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed open: there is no session to end, and no other call. The bytes of the sector do not change. */
	chip_start(1);
	chip.bytes[10] = 0;
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_OPEN && chip.calls == 1 && ran_bits(&report) == BIT(FWT_OPEN));
	CHECK(report.status[FWT_OPEN] == CHIP_ERROR && chip.bytes[10] == 0);

	/* The sector is not erased at the first read: the session ends, and nothing is written. */
	chip_start(0);
	chip.bytes[10] = 0;
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_FIRST && chip.calls == 4 && ran_bits(&report) == RAN_SESSION);
	CHECK(chip.bytes[10] == 0 && report.as_expected[FWT_CLOSE] == 1 && report.as_expected[FWT_READ_OUTSIDE] == 0);

	/* A first read that fails: the same steps. */
	chip_start(2);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_FIRST && chip.calls == 4 && ran_bits(&report) == RAN_SESSION);
	CHECK(report.status[FWT_READ_FIRST] == CHIP_ERROR && flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed erase: the session ends before the write. */
	chip_start(3);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_ERASE && chip.calls == 5 && ran_bits(&report) == (RAN_SESSION | BIT(FWT_ERASE)));
	CHECK(flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* An erase that changes nothing. The first steps pass on an erased sector. The erase-write then programs its
	 * pattern over the first one, and the two last reads do not find an erased sector. */
	chip_start(0);
	chip.no_erase = 1;
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_ERASED_WRITTEN && chip.calls == FWT_STEPS && ran_bits(&report) == RAN_ALL);
	CHECK(report.as_expected[FWT_ERASE_WRITE] == 1 && report.as_expected[FWT_READ_FINAL] == 0);
	CHECK(report.as_expected[FWT_READ_OUTSIDE] == 0 && !flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed write: the last erase follows, and the sector is erased at the end. */
	chip_start(5);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_WRITE && chip.calls == 9 && report.status[FWT_WRITE] == CHIP_ERROR);
	CHECK(ran_bits(&report) == (RAN_ALL & ~(BIT(FWT_READ_WRITTEN) | BIT(FWT_ERASE_WRITE) | BIT(FWT_READ_ERASED_WRITTEN))));
	CHECK(flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A write that changes nothing: the read after it is not as expected. */
	chip_start(0);
	chip.no_write = 1;
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_WRITTEN && chip.calls == 10);
	CHECK(ran_bits(&report) == (RAN_ALL & ~(BIT(FWT_ERASE_WRITE) | BIT(FWT_READ_ERASED_WRITTEN))));
	CHECK(flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed erase-write: the last erase follows. */
	chip_start(7);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_ERASE_WRITE && chip.calls == 11 && report.status[FWT_ERASE_WRITE] == CHIP_ERROR);
	CHECK(ran_bits(&report) == (RAN_ALL & ~BIT(FWT_READ_ERASED_WRITTEN)));
	CHECK(flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed read after the erase-write: the last erase follows. */
	chip_start(8);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_ERASED_WRITTEN && chip.calls == FWT_STEPS && ran_bits(&report) == RAN_ALL);
	CHECK(report.status[FWT_READ_ERASED_WRITTEN] == CHIP_ERROR && flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* The last erase fails: the sector keeps the second pattern, and the two reads that follow say so. */
	chip_start(9);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_ERASE_LAST && chip.calls == FWT_STEPS && ran_bits(&report) == RAN_ALL);
	CHECK(report.status[FWT_ERASE_LAST] == CHIP_ERROR && !memcmp(chip.bytes, b, FLASH_SECTOR_BYTES));
	CHECK(report.as_expected[FWT_READ_FINAL] == 0 && report.as_expected[FWT_CLOSE] == 1 && report.as_expected[FWT_READ_OUTSIDE] == 0);

	/* A failed close: each step ran, and the last read still finds the erased sector. */
	chip_start(11);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_CLOSE && chip.calls == FWT_STEPS && ran_bits(&report) == RAN_ALL);
	CHECK(report.as_expected[FWT_READ_OUTSIDE] == 1 && flash_block_is_erased(chip.bytes, FLASH_SECTOR_BYTES));

	/* A failed last read. */
	chip_start(12);
	CHECK(!flash_run_write_test(&ops, TEST_SECTOR, &report));
	CHECK(report.failed == FWT_READ_OUTSIDE && chip.calls == FWT_STEPS && report.status[FWT_READ_OUTSIDE] == CHIP_ERROR);
}

unsigned flash_tests(const char *stock_image, unsigned *check_count)
{
	unsigned char *image = malloc(FLASH_IMAGE_BYTES), digest[FLASH_DIGEST_BYTES];
	struct flash_image info, other;
	struct flash_volume volume;
	unsigned long value;
	char hex[FLASH_DIGEST_BYTES * 2u + 1u];
	unsigned char *stock;
	size_t bytes = 0;

	checks = 0;
	failures = 0;
	if (image == NULL) {
		CHECK(image != NULL);
		*check_count = checks;
		return failures;
	}
	build_image(image);

	/* The three digests against the library itself. */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	efl_sha256(image, FLASH_IMAGE_BYTES, digest);
	CHECK(same(digest, info.sha256));
	expected_base(image, FLASH_IMAGE_BYTES, digest);
	CHECK(same(digest, info.base));
	efl_sha256(image, FLASH_STORE_BYTES, digest);
	CHECK(same(digest, info.store));

	/* The volumes: the pad files count as used, and the file marked for update is valid. */
	CHECK(info.store_volume.present && info.store_volume.bytes == FLASH_STORE_BYTES && info.store_volume.header_bytes == 0x48);
	CHECK(info.store_volume.attributes == 0x4FEFF && info.store_volume.used_end == FLASH_STORE_BYTES && !info.store_volume.stream.present);
	CHECK(info.outer_volume.present && info.outer_volume.bytes == 0x320000 && info.outer_volume.used_end == FLASH_OUTER_OFFSET + PAD_AT + PAD_BYTES);
	CHECK(info.pei_volume.present && info.pei_volume.attributes == 0xCFEFF && info.pei_volume.used_end == FLASH_PEI_OFFSET + 0x1478);
	/* The streams: the pad file is skipped. */
	CHECK(info.outer_volume.stream.present && info.outer_volume.stream.first == FLASH_OUTER_OFFSET + FIRST_FILE + FFS_HEADER_BYTES + SECTION_GUIDED_BYTES + LZMA_HEADER_BYTES);
	CHECK(info.outer_volume.stream.compressed == STREAM_BYTES && info.outer_volume.stream.plain == 0x418000);
	CHECK(!info.pei_volume.stream.present);
	CHECK(flash_volume_at(image, FLASH_IMAGE_BYTES, FLASH_OUTER_OFFSET, &volume) && volume.used_end == info.outer_volume.used_end);
	CHECK(volume.stream.present && volume.stream.first == info.outer_volume.stream.first);

	/* A byte of the variable store changes the digest of the image and that of the store, and not the base. */
	image[0x100] ^= 1u;
	flash_image_describe(image, FLASH_IMAGE_BYTES, &other);
	CHECK(same(other.base, info.base) && !same(other.sha256, info.sha256) && !same(other.store, info.store));
	/* A byte outside the store changes the base. */
	build_image(image);
	image[0x400000] ^= 1u;
	flash_image_describe(image, FLASH_IMAGE_BYTES, &other);
	CHECK(!same(other.base, info.base) && same(other.store, info.store));
	CHECK(other.outer_volume.used_end == FLASH_OUTER_OFFSET + PAD_AT + PAD_BYTES);

	/* No signature at the PEI volume: it is not present, and the other volumes are read. */
	build_image(image);
	image[FLASH_PEI_OFFSET + 0x28] = 'X';
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.pei_volume.present && info.outer_volume.present && info.store_volume.present);
	/* A length over the end of the image, and a header longer than its volume: not present. */
	build_image(image);
	put64(image + FLASH_OUTER_OFFSET + 0x20, FLASH_IMAGE_BYTES);
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.outer_volume.present && info.pei_volume.present);
	build_image(image);
	put16(image + FLASH_PEI_OFFSET + 0x30, 0x50);
	put64(image + FLASH_PEI_OFFSET + 0x20, 0x40);
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.pei_volume.present);
	/* A volume that the image cuts off: there is no header to read. */
	build_image(image);
	CHECK(!flash_volume_at(image, FLASH_OUTER_OFFSET + 0x30, FLASH_OUTER_OFFSET, &volume) && volume.present == 0);
	CHECK(!flash_volume_at(image, FLASH_OUTER_OFFSET, FLASH_OUTER_OFFSET, &volume));

	/* The walk stops at a deleted file, at an invalid file, at free space and at erased space. */
	build_image(image);
	image[FLASH_PEI_OFFSET + 0x478 + 23] = (unsigned char)~0x17u;	/* the second file: deleted */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(info.pei_volume.used_end == FLASH_PEI_OFFSET + 0x478);
	build_image(image);
	image[FLASH_PEI_OFFSET + 0x478 + 23] = (unsigned char)~0x27u;	/* the second file: its header is invalid */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(info.pei_volume.used_end == FLASH_PEI_OFFSET + 0x478);
	build_image(image);
	image[FLASH_OUTER_OFFSET + PAD_AT + 20] = image[FLASH_OUTER_OFFSET + PAD_AT + 21] = image[FLASH_OUTER_OFFSET + PAD_AT + 22] = 0xFF;
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	/* The used end is the end of the last valid file: it is on no boundary. */
	CHECK(info.outer_volume.used_end == FLASH_OUTER_OFFSET + FIRST_FILE + OUTER_FILE_BYTES);
	build_image(image);
	memset(image + FLASH_OUTER_OFFSET + PAD_AT, 0xFF, FFS_HEADER_BYTES);
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(info.outer_volume.used_end == FLASH_OUTER_OFFSET + FIRST_FILE + OUTER_FILE_BYTES);

	/* The stream needs an image file with a guided LZMA section. */
	build_image(image);
	image[FLASH_OUTER_OFFSET + FIRST_FILE + FFS_HEADER_BYTES + 3] = 0x01;	/* a compression section, not guided */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.outer_volume.stream.present && info.outer_volume.present && info.outer_volume.used_end == FLASH_OUTER_OFFSET + PAD_AT + PAD_BYTES);
	build_image(image);
	image[FLASH_OUTER_OFFSET + FIRST_FILE + 18] = 0x07;			/* a driver, not an image file */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.outer_volume.stream.present);
	build_image(image);
	memset(image + FLASH_OUTER_OFFSET + FIRST_FILE, 0xFF, FLASH_SECTOR_BYTES);	/* only the pad file: the rest is free */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.outer_volume.stream.present && info.outer_volume.used_end == FLASH_OUTER_OFFSET + 0x74);
	build_image(image);
	put16(image + FLASH_OUTER_OFFSET + FIRST_FILE + FFS_HEADER_BYTES + 20, 8u);	/* a data offset inside the section header */
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(!info.outer_volume.stream.present);

	/* A short image describes what fits: the base of its bytes, and the outer volume cut off. */
	build_image(image);
	flash_image_describe(image, FLASH_STORE_BYTES + 0x1000, &other);
	expected_base(image, FLASH_STORE_BYTES + 0x1000, digest);
	CHECK(same(digest, other.base) && other.store_volume.present && other.store_volume.used_end == FLASH_STORE_BYTES);
	CHECK(!other.outer_volume.present);
	flash_image_describe(image, 0, &other);
	expected_base(image, 0, digest);
	CHECK(same(digest, other.base) && !other.store_volume.present);
	efl_sha256("", 0, digest);
	CHECK(same(digest, other.sha256) && same(digest, other.store));

	/* The text of a digest. */
	flash_digest_text(info.sha256, hex);
	CHECK(strlen(hex) == FLASH_DIGEST_BYTES * 2u && strspn(hex, "0123456789abcdef") == FLASH_DIGEST_BYTES * 2u);

	/* ---- the erased state of a block, where a sector may be written, and the text of a number ---- */

	CHECK(flash_block_is_erased(info.sha256, 0) && !flash_block_is_erased(info.sha256, FLASH_DIGEST_BYTES));
	memset(image, 0xFF, 64);
	CHECK(flash_block_is_erased(image, 64));
	image[63] = 0xFE;
	CHECK(!flash_block_is_erased(image, 64));

	/* The image file of the outer volume ends at 0xAE04C0: the first whole free sector is at 0xAE1000. */
	build_image(image);
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(info.outer_volume.used_end == FLASH_OUTER_OFFSET + PAD_AT + PAD_BYTES);
	CHECK(flash_sector_place(&info, 0xAE1000) == FLASH_SECTOR_FREE && flash_sector_place(&info, 0xDFF000) == FLASH_SECTOR_FREE);
	CHECK(flash_sector_place(&info, 0xAE0000) == FLASH_SECTOR_VOLUME_START);
	CHECK(flash_sector_place(&info, 0xAE0001) == FLASH_SECTOR_UNALIGNED && flash_sector_place(&info, 0x123) == FLASH_SECTOR_UNALIGNED);
	CHECK(flash_sector_place(&info, 0x1000) == FLASH_SECTOR_OUTSIDE && flash_sector_place(&info, 0x20000) == FLASH_SECTOR_OUTSIDE);
	CHECK(flash_sector_place(&info, 0xE00000) == FLASH_SECTOR_OUTSIDE && flash_sector_place(&info, 0x1000000) == FLASH_SECTOR_OUTSIDE);
	/* A last file of two sectors: the sectors that hold it are in use, and the next one is free. */
	build_image(image);
	put_file(image, FLASH_OUTER_OFFSET + PAD_AT, 0xF0, 0x2000, (unsigned char)~0x07u);
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(info.outer_volume.used_end == FLASH_OUTER_OFFSET + PAD_AT + 0x2000);
	CHECK(flash_sector_place(&info, 0xAE1000) == FLASH_SECTOR_IN_USE && flash_sector_place(&info, 0xAE2000) == FLASH_SECTOR_IN_USE);
	CHECK(flash_sector_place(&info, 0xAE3000) == FLASH_SECTOR_FREE && flash_sector_place(&info, 0xAE0000) == FLASH_SECTOR_VOLUME_START);
	/* Without a sound header of the outer volume no sector is a place for a write. */
	build_image(image);
	image[FLASH_OUTER_OFFSET + 0x28] = 'X';
	flash_image_describe(image, FLASH_IMAGE_BYTES, &info);
	CHECK(flash_sector_place(&info, 0xAE1000) == FLASH_SECTOR_NO_VOLUME);
	CHECK(!strcmp(flash_sector_place_text(FLASH_SECTOR_FREE), "free") && flash_sector_place_text(FLASH_SECTOR_OUTSIDE)[0] != 0);

	value = 7;
	CHECK(flash_parse_hex("0xC24000", &value) && value == 0xC24000);
	CHECK(flash_parse_hex("c24000", &value) && value == 0xC24000);
	CHECK(flash_parse_hex("0X001000", &value) && value == 0x1000);
	/* A text that is not one number leaves the value as it was. */
	value = 7;
	CHECK(!flash_parse_hex("", &value) && value == 7 && !flash_parse_hex("0x", &value) && value == 7);
	CHECK(!flash_parse_hex("12g4", &value) && !flash_parse_hex("-4", &value) && !flash_parse_hex(" 10", &value) && value == 7);
	CHECK(!flash_parse_hex("100000000", &value) && value == 7);

	test_write_test();

	/* The stock image of the vendor. */
	stock = stock_image != NULL ? efl_test_load("", stock_image, &bytes) : NULL;
	if (stock == NULL) printf("SKIP flash: the stock image %s is not there\n", stock_image ? stock_image : "(no file given)");
	else {
		CHECK(bytes == FLASH_IMAGE_BYTES);
		flash_image_describe(stock, bytes, &info);
		flash_digest_text(info.sha256, hex);
		printf("stock image: sha256 %s\n", hex);
		/* The digest that the provenance file of the workspace names. */
		CHECK(!strcmp(hex, "07595ca3aecf8a4caa28a397b5298f3946a1b769f87b16f67adc369c3f69045c"));
		flash_digest_text(info.base, hex);
		printf("stock image: base %s\n", hex);
		flash_digest_text(info.store, hex);
		printf("stock image: store %s\n", hex);
		/* Read in the bytes of the stock image: the store volume is 0x20000 bytes and full, the outer volume is
		 * 0x320000 bytes with its image file to 0x141E7A and free space behind it, and the PEI volume is 0x200000
		 * bytes with one image file to its end. The stream of the outer volume declares 0x418010 plain bytes
		 * (P6-L1-INTEGRATION, section 1, of the workspace, for the sizes). */
		CHECK(info.store_volume.present && info.store_volume.bytes == FLASH_STORE_BYTES && info.store_volume.used_end == FLASH_STORE_BYTES);
		CHECK(!info.store_volume.stream.present);
		CHECK(info.outer_volume.present && info.outer_volume.bytes == 0x320000 && info.outer_volume.used_end == 0xC21E7A);
		CHECK(info.outer_volume.stream.present && info.outer_volume.stream.plain == 0x418010);
		CHECK(info.pei_volume.present && info.pei_volume.bytes == 0x200000 && info.pei_volume.used_end == FLASH_IMAGE_BYTES);
		/* The image file of the PEI volume has no LZMA section. */
		CHECK(!info.pei_volume.stream.present);
		printf("stock image: outer stream at 0x%lX, %lu compressed bytes, %llu plain bytes\n", info.outer_volume.stream.first,
		       info.outer_volume.stream.compressed, info.outer_volume.stream.plain);
		free(stock);
	}
	free(image);
	*check_count = checks;
	return failures;
}
