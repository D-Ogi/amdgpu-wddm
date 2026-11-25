/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the description of the firmware image (flash.h): the three digests, the firmware volumes and the
 * LZMA streams. The first part runs on an image that this test builds, with the file layout of the stock image: each
 * volume starts with a pad file for the extended header. The second part runs on the stock image of the vendor,
 * which is outside the repository, and says SKIP when it is not there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efl_test.h"
#include "efl_sha256.h"
#include "flash.h"

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

unsigned flash_tests(const char *stock_image, unsigned *check_count)
{
	unsigned char *image = malloc(FLASH_IMAGE_BYTES), digest[FLASH_DIGEST_BYTES];
	struct flash_image info, other;
	struct flash_volume volume;
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
