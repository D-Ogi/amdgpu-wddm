/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The firmware image of the BC-250: its identity and its volumes. The contract is in flash.h. */
#include <string.h>
#include "flash.h"
#include "efl_sha256.h"

/* The firmware volume header (UEFI 2.10, section 3.2.1): FvLength at 0x20, the signature at 0x28, the attributes
 * at 0x2C, HeaderLength at 0x30, then the block map. The variable part starts at 0x38. */
#define FV_FIXED_BYTES		0x38u
#define FV_SIGNATURE		"_FVH"
#define FV_ERASE_POLARITY	0x800u		/* EFI_FVB2_ERASE_POLARITY: a bit set in the flash reads as clear */

/* The file header of a firmware file system (UEFI 2.10, section 3.2.2): Type at 18, Size at 20 (three bytes), State
 * at 23. A file is valid when its header and its data are valid, and it is neither deleted nor invalid. The bits of
 * construction and of "marked for update" do not matter: the file of the variable store of the stock image has the
 * second one set. */
#define FFS_HEADER_BYTES	24u
#define FFS_TYPE_PAD		0xF0u		/* EFI_FV_FILETYPE_FFS_PAD */
#define FFS_TYPE_IMAGE		0x0Bu		/* EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE */
#define FFS_FREE_SIZE		0xFFFFFFu	/* no file from here to the end of the volume */
#define FFS_STATE_NEEDED	0x06u		/* HEADER_VALID and DATA_VALID */
#define FFS_STATE_FORBIDDEN	0x30u		/* DELETED and HEADER_INVALID */

/* A guided section (UEFI 2.10, section 3.2.4): Size at 0 (three bytes), Type at 3, the GUID at 4, DataOffset at
 * 20. The data of the LZMA section starts with 5 bytes of properties and 8 bytes of the plain size. */
#define SECTION_GUIDED_BYTES	24u
#define SECTION_TYPE_GUIDED	0x02u
#define LZMA_HEADER_BYTES	13u
static const unsigned char lzma_guid[16] = {	/* EE4E5898-3914-4259-9D6E-DC7BD79403CF */
	0x98, 0x58, 0x4E, 0xEE, 0x14, 0x39, 0x59, 0x42, 0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF
};

/* Zeros for the digest of a base. The loader of the firmware clears the memory of a static object. */
static unsigned char zeros[FLASH_SECTOR_BYTES];

static unsigned le24(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
}

static unsigned le16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned le32(const unsigned char *p)
{
	return le16(p) | (le16(p + 2) << 16);
}

static unsigned long long le64(const unsigned char *p)
{
	return (unsigned long long)le32(p) | ((unsigned long long)le32(p + 4) << 32);
}

static int one_value(const unsigned char *p, size_t bytes, unsigned char value)
{
	size_t i;
	for (i = 0; i < bytes; i++)
		if (p[i] != value) return 0;
	return 1;
}

/* The image offset after the last valid file of the volume, or 0. The walk stops at a file that is free, erased, not
 * valid or too short. */
static unsigned long used_end(const unsigned char *image, unsigned long limit, unsigned long at, int inverted)
{
	unsigned long last = 0;
	while (at + FFS_HEADER_BYTES <= limit) {
		unsigned long size = le24(image + at + 20);
		unsigned char state = image[at + 23];
		if (size == FFS_FREE_SIZE || one_value(image + at, FFS_HEADER_BYTES, 0xFF)) break;
		if (inverted) state = (unsigned char)~state;
		if ((state & FFS_STATE_NEEDED) != FFS_STATE_NEEDED || (state & FFS_STATE_FORBIDDEN) != 0) break;
		if (size < FFS_HEADER_BYTES) break;
		last = at + size;
		at = (last + 7u) & ~7ul;
	}
	return last;
}

/* The LZMA stream in the first file of the volume that is not a pad file. */
static void read_stream(const unsigned char *image, unsigned long limit, unsigned long at, struct flash_stream *out)
{
	unsigned long section, size, data;
	while (at + FFS_HEADER_BYTES <= limit) {
		size = le24(image + at + 20);
		if (image[at + 18] != FFS_TYPE_PAD) break;
		/* A pad file is valid: its size is sound. */
		at = (at + (size < FFS_HEADER_BYTES ? FFS_HEADER_BYTES : size) + 7u) & ~7ul;
	}
	if (at + FFS_HEADER_BYTES > limit || image[at + 18] != FFS_TYPE_IMAGE) return;
	if (size < FFS_HEADER_BYTES + SECTION_GUIDED_BYTES || size > limit - at) return;
	section = at + FFS_HEADER_BYTES;
	if (image[section + 3] != SECTION_TYPE_GUIDED || memcmp(image + section + 4, lzma_guid, 16) != 0) return;
	data = le16(image + section + 20);
	if (data < SECTION_GUIDED_BYTES || data + LZMA_HEADER_BYTES > size - FFS_HEADER_BYTES) return;
	out->present = 1;
	out->first = section + data + LZMA_HEADER_BYTES;
	out->compressed = size - FFS_HEADER_BYTES - data - LZMA_HEADER_BYTES;
	out->plain = le64(image + section + data + 5);
}

int flash_volume_at(const unsigned char *image, size_t bytes, unsigned long offset, struct flash_volume *out)
{
	unsigned long long length;
	unsigned long header, limit;
	int inverted;
	memset(out, 0, sizeof(*out));
	if (offset > bytes || bytes - offset < FV_FIXED_BYTES) return 0;
	if (memcmp(image + offset + 0x28, FV_SIGNATURE, 4) != 0) return 0;
	length = le64(image + offset + 0x20);
	header = le16(image + offset + 0x30);
	if (header < FV_FIXED_BYTES || length < header || length > bytes - offset) return 0;
	out->present = 1;
	out->bytes = length;
	out->header_bytes = header;
	out->attributes = le32(image + offset + 0x2C);
	limit = offset + (unsigned long)length;
	inverted = (out->attributes & FV_ERASE_POLARITY) != 0;
	out->used_end = used_end(image, limit, offset + header, inverted);
	read_stream(image, limit, offset + header, &out->stream);
	return 1;
}

void flash_image_describe(const unsigned char *image, size_t bytes, struct flash_image *out)
{
	struct efl_sha256 hash;
	size_t store = bytes < FLASH_STORE_BYTES ? bytes : FLASH_STORE_BYTES, at;
	memset(out, 0, sizeof(*out));
	efl_sha256(image, bytes, out->sha256);
	efl_sha256_init(&hash);
	for (at = 0; at < store; at += sizeof(zeros))
		efl_sha256_update(&hash, zeros, store - at < sizeof(zeros) ? store - at : sizeof(zeros));
	if (bytes > store) efl_sha256_update(&hash, image + store, bytes - store);
	efl_sha256_final(&hash, out->base);
	efl_sha256(image, store, out->store);
	(void)flash_volume_at(image, bytes, FLASH_STORE_OFFSET, &out->store_volume);
	(void)flash_volume_at(image, bytes, FLASH_OUTER_OFFSET, &out->outer_volume);
	(void)flash_volume_at(image, bytes, FLASH_PEI_OFFSET, &out->pei_volume);
}

void flash_digest_text(const unsigned char digest[FLASH_DIGEST_BYTES], char *out)
{
	static const char digits[] = "0123456789abcdef";
	unsigned i;
	for (i = 0; i < FLASH_DIGEST_BYTES; i++) {
		out[2u * i] = digits[digest[i] >> 4];
		out[2u * i + 1u] = digits[digest[i] & 15u];
	}
	out[2u * FLASH_DIGEST_BYTES] = 0;
}

int flash_block_is_erased(const unsigned char *block, size_t bytes)
{
	return one_value(block, bytes, 0xFF);
}

enum flash_sector_place flash_sector_place(const struct flash_image *info, unsigned long offset)
{
	const struct flash_volume *outer = &info->outer_volume;
	unsigned long long end, used;
	if (offset % FLASH_SECTOR_BYTES != 0) return FLASH_SECTOR_UNALIGNED;
	if (!outer->present) return FLASH_SECTOR_NO_VOLUME;
	end = (unsigned long long)FLASH_OUTER_OFFSET + outer->bytes;
	/* The store and the PSP area are before the outer volume: no job of this module writes there. */
	if (offset < FLASH_OUTER_OFFSET || offset + FLASH_SECTOR_BYTES > end) return FLASH_SECTOR_OUTSIDE;
	if (offset == FLASH_OUTER_OFFSET) return FLASH_SECTOR_VOLUME_START;
	/* The file ends inside a sector: that sector is in use up to its last byte. */
	used = (outer->used_end + FLASH_SECTOR_BYTES - 1u) & ~(unsigned long)(FLASH_SECTOR_BYTES - 1u);
	if (offset < used) return FLASH_SECTOR_IN_USE;
	return FLASH_SECTOR_FREE;
}

const char *flash_sector_place_text(enum flash_sector_place place)
{
	switch (place) {
	case FLASH_SECTOR_FREE: return "free";
	case FLASH_SECTOR_UNALIGNED: return "not a multiple of 4096";
	case FLASH_SECTOR_NO_VOLUME: return "the outer volume has no sound header";
	case FLASH_SECTOR_VOLUME_START: return "the first sector of the outer volume";
	case FLASH_SECTOR_IN_USE: return "inside a file of the outer volume";
	default: return "outside the outer volume";
	}
}

int flash_parse_hex(const char *text, unsigned long *out)
{
	unsigned long value = 0;
	unsigned digits = 0;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
	for (; *text; text++) {
		unsigned char c = (unsigned char)*text, digit;
		if (c >= '0' && c <= '9') digit = (unsigned char)(c - '0');
		else if (c >= 'a' && c <= 'f') digit = (unsigned char)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') digit = (unsigned char)(c - 'A' + 10);
		else return 0;
		/* An image offset has at most eight hexadecimal digits. A longer number is not one. */
		if (++digits > 8u) return 0;
		value = value * 16u + digit;
	}
	if (digits == 0) return 0;
	*out = value;
	return 1;
}