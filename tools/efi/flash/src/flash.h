/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The firmware image of the BC-250 as the flash job of the stage reads it (docs/design/p6-flash.md).
 *
 * The image is the 16 MiB of the SPI flash of the board. Four parts of it are named here:
 *
 *   0x000000-0x01FFFF  the firmware volume that holds the NVAR variable store
 *   0x020000-0xADFFFF  the PSP directories and entries
 *   0xAE0000-0xDFFFFF  the outer firmware volume with the compressed nested volume of the DXE and SMM modules
 *   0xE00000-0xFFFFFF  the outer firmware volume with the nested volume of the PEI modules
 *
 * Each volume starts with a pad file that holds the extended header of the volume. The PEI volume ends at the end of
 * the image: the reset vector is in its last bytes.
 *
 * The variable store holds the bytes that are the unit's own. On unit A the rest of the image is equal to the
 * image file of the vendor (fact M965; one unit). So the name of a base is the SHA-256 of the image in which the
 * variable store reads as zeros.
 *
 * flash_image.c and flash_test.c are pure code. They build for the stage and for the host tests.
 */
#ifndef FLASH_H
#define FLASH_H
#include <stddef.h>

#define FLASH_IMAGE_BYTES	(16u * 1024u * 1024u)
#define FLASH_STORE_BYTES	0x20000u
#define FLASH_SECTOR_BYTES	0x1000u
#define FLASH_ROM_BASE		0xFF000000ull	/* the address at which the firmware maps the image */
#define FLASH_STORE_OFFSET	0x0ul
#define FLASH_OUTER_OFFSET	0xAE0000ul
#define FLASH_PEI_OFFSET	0xE00000ul
#define FLASH_DIGEST_BYTES	32u

/* The compressed stream of a nested volume, in the first file of a volume that is not a pad file. */
struct flash_stream {
	int present;			/* 0: that file is not an image file with an LZMA section */
	unsigned long first;		/* the image offset of the first compressed byte */
	unsigned long compressed;	/* the bytes of the stream, from the section header */
	unsigned long long plain;	/* the bytes of the decompressed data, from the stream header */
};

/* A firmware volume of the image (UEFI 2.10, section 3.2). */
struct flash_volume {
	int present;			/* 0: there is no firmware volume header with a sound length at the offset */
	unsigned long long bytes;	/* FvLength */
	unsigned long header_bytes;	/* HeaderLength */
	unsigned attributes;		/* the attributes of the header */
	unsigned long used_end;		/* the image offset after the last valid file; 0 when no file is valid */
	struct flash_stream stream;
};

struct flash_image {
	unsigned char sha256[FLASH_DIGEST_BYTES];	/* the whole image */
	unsigned char base[FLASH_DIGEST_BYTES];		/* the image with the variable store read as zeros */
	unsigned char store[FLASH_DIGEST_BYTES];	/* the variable store alone */
	struct flash_volume store_volume, outer_volume, pei_volume;
};

/* Reads the identity and the volumes of an image. bytes is FLASH_IMAGE_BYTES for an image of the board. A smaller
 * count describes what fits: a volume or a part that is cut off is not present. */
void flash_image_describe(const unsigned char *image, size_t bytes, struct flash_image *out);

/* The firmware volume at an image offset, with its used end and its stream. 1 when there is a firmware volume
 * header with a sound length. */
int flash_volume_at(const unsigned char *image, size_t bytes, unsigned long offset, struct flash_volume *out);

/* The lower-case hex text of a digest. out takes FLASH_DIGEST_BYTES * 2 + 1 characters. */
void flash_digest_text(const unsigned char digest[FLASH_DIGEST_BYTES], char *out);

/* 1 when every byte is 0xFF: the state of a sector after an erase. */
int flash_block_is_erased(const unsigned char *block, size_t bytes);

/* ---- where a sector may be written ------------------------------------------------------------------ */

/* Where a sector of FLASH_SECTOR bytes stands in the image. Only a free sector is a place for a write that is not
 * the update of the image: a byte behind the last file of the outer volume. The first sector of a volume holds the
 * header of the volume, and the flash protocol has its own bookkeeping for it. */
enum flash_sector_place {
	FLASH_SECTOR_FREE = 0,
	FLASH_SECTOR_UNALIGNED,		/* not a multiple of FLASH_SECTOR_BYTES */
	FLASH_SECTOR_NO_VOLUME,		/* the outer volume has no sound header */
	FLASH_SECTOR_VOLUME_START,	/* the first sector of the outer volume */
	FLASH_SECTOR_IN_USE,		/* inside a file of the outer volume, or behind the end of the volume */
	FLASH_SECTOR_OUTSIDE		/* in the variable store or in the PSP area */
};
enum flash_sector_place flash_sector_place(const struct flash_image *info, unsigned long offset);
const char *flash_sector_place_text(enum flash_sector_place place);

/* Parses a hexadecimal number with an optional 0x: an image offset in a load option. 1 when the whole text is one. */
int flash_parse_hex(const char *text, unsigned long *out);

#endif
