/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The Authenticode digest of a PE image (sb_pe.h). Each offset below is named once, from the "PE Format"
 * specification. */
#include <stddef.h>
#include <string.h>
#include "efl_sha256.h"
#include "sb_pe.h"

#define DOS_MAGIC		0x5A4Du		/* "MZ" */
#define DOS_LFANEW		0x3Cu		/* the file offset of the PE signature */
#define PE_SIGNATURE_BYTES	4u
#define COFF_HEADER_BYTES	20u
#define COFF_SECTIONS		2u		/* NumberOfSections */
#define COFF_OPTIONAL_BYTES	16u		/* SizeOfOptionalHeader */
#define OPT_MAGIC_PE32		0x10Bu
#define OPT_MAGIC_PE32_PLUS	0x20Bu
#define OPT_SIZE_OF_HEADERS	60u
#define OPT_CHECKSUM		64u
#define OPT32_DIRECTORY_COUNT	92u		/* NumberOfRvaAndSizes */
#define OPT32_DIRECTORIES	96u
#define OPT64_DIRECTORY_COUNT	108u
#define OPT64_DIRECTORIES	112u
#define DIRECTORY_BYTES		8u
#define DIRECTORY_SECURITY	4u		/* the Certificate Table entry: a file offset and a size */
#define SECTION_BYTES		40u
#define SECTION_RAW_SIZE	16u
#define SECTION_RAW_POINTER	20u
#define MAX_SECTIONS		96u		/* the Windows loader's limit */

/* WIN_CERTIFICATE: dwLength, wRevision, wCertificateType, then the certificate. */
#define WIN_CERT_HEADER_BYTES	8u
#define WIN_CERT_REVISION_2_0	0x0200u
#define WIN_CERT_PKCS_SIGNED	0x0002u

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static size_t le32(const unsigned char *p)
{
	return (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
}

int sb_pe_read(const unsigned char *image, size_t bytes, struct sb_pe *out)
{
	struct efl_sha256 hash;
	size_t pe, opt, opt_bytes, headers, checksum, directories, directory_count, security, sections_at, sum, end;
	unsigned sections, order[MAX_SECTIONS], i, j, magic;

	memset(out, 0, sizeof(*out));
	if (image == NULL || bytes < DOS_LFANEW + 4u || le16(image) != DOS_MAGIC) return 0;
	pe = le32(image + DOS_LFANEW);
	if (pe > bytes || bytes - pe < PE_SIGNATURE_BYTES + COFF_HEADER_BYTES || memcmp(image + pe, "PE\0\0", 4) != 0)
		return 0;
	sections = le16(image + pe + PE_SIGNATURE_BYTES + COFF_SECTIONS);
	opt_bytes = le16(image + pe + PE_SIGNATURE_BYTES + COFF_OPTIONAL_BYTES);
	opt = pe + PE_SIGNATURE_BYTES + COFF_HEADER_BYTES;
	if (sections > MAX_SECTIONS || opt_bytes > bytes - opt || opt_bytes < OPT32_DIRECTORIES) return 0;
	magic = le16(image + opt);
	if (magic == OPT_MAGIC_PE32) {
		directory_count = le32(image + opt + OPT32_DIRECTORY_COUNT);
		directories = opt + OPT32_DIRECTORIES;
	} else if (magic == OPT_MAGIC_PE32_PLUS && opt_bytes >= OPT64_DIRECTORIES) {
		directory_count = le32(image + opt + OPT64_DIRECTORY_COUNT);
		directories = opt + OPT64_DIRECTORIES;
	} else
		return 0;
	/* The data directories must lie inside the optional header. */
	if (directory_count > (opt + opt_bytes - directories) / DIRECTORY_BYTES) return 0;
	headers = le32(image + opt + OPT_SIZE_OF_HEADERS);
	sections_at = opt + opt_bytes;
	if (headers > bytes || (size_t)sections * SECTION_BYTES > bytes - sections_at ||
	    headers < sections_at + (size_t)sections * SECTION_BYTES)
		return 0;
	checksum = opt + OPT_CHECKSUM;
	end = bytes;
	if (directory_count > DIRECTORY_SECURITY) {
		security = directories + DIRECTORY_SECURITY * DIRECTORY_BYTES;
		out->cert_offset = le32(image + security);
		out->cert_bytes = le32(image + security + 4u);
		if (out->cert_bytes) {
			if (out->cert_offset > bytes || bytes - out->cert_offset != out->cert_bytes || out->cert_offset < headers)
				return 0;
			end = out->cert_offset;
		}
	} else
		security = 0;

	/* The headers, without the checksum and without the Certificate Table entry. */
	efl_sha256_init(&hash);
	efl_sha256_update(&hash, image, checksum);
	if (security) {
		efl_sha256_update(&hash, image + checksum + 4u, security - (checksum + 4u));
		efl_sha256_update(&hash, image + security + DIRECTORY_BYTES, headers - (security + DIRECTORY_BYTES));
	} else
		efl_sha256_update(&hash, image + checksum + 4u, headers - (checksum + 4u));

	/* The sections that have raw data, in the order of their file offsets. */
	for (i = 0, j = 0; i < sections; i++)
		if (le32(image + sections_at + i * SECTION_BYTES + SECTION_RAW_SIZE) != 0) order[j++] = i;
	sections = j;
	for (i = 1; i < sections; i++) {
		unsigned moved = order[i];
		size_t at = le32(image + sections_at + moved * SECTION_BYTES + SECTION_RAW_POINTER);
		for (j = i; j > 0 && le32(image + sections_at + order[j - 1u] * SECTION_BYTES + SECTION_RAW_POINTER) > at; j--)
			order[j] = order[j - 1u];
		order[j] = moved;
	}
	sum = headers;
	for (i = 0; i < sections; i++) {
		const unsigned char *section = image + sections_at + order[i] * SECTION_BYTES;
		size_t at = le32(section + SECTION_RAW_POINTER), size = le32(section + SECTION_RAW_SIZE);
		/* Inside the file and before the certificate table. The sum of the sizes can pass the end only when
		 * two sections share bytes, which the next check refuses. */
		if (at > end || size > end - at) return 0;
		efl_sha256_update(&hash, image + at, size);
		sum += size;
		if (sum > end) return 0;
	}
	/* The bytes after the last section, up to the certificate table. */
	efl_sha256_update(&hash, image + sum, end - sum);
	efl_sha256_final(&hash, out->digest);
	return 1;
}

int sb_pe_next_signature(const unsigned char *image, const struct sb_pe *pe, size_t *cursor,
			 const unsigned char **signature, size_t *signature_bytes)
{
	while (*cursor < pe->cert_bytes) {
		const unsigned char *entry = image + pe->cert_offset + *cursor;
		size_t left = pe->cert_bytes - *cursor, length, padded;
		if (left < WIN_CERT_HEADER_BYTES) return -1;
		length = le32(entry);
		if (length < WIN_CERT_HEADER_BYTES || length > left) return -1;
		/* Each entry starts on an 8-byte boundary. The last one may end without its padding. */
		padded = (length + 7u) & ~(size_t)7u;
		*cursor += padded > left ? left : padded;
		if (le16(entry + 4) != WIN_CERT_REVISION_2_0 || le16(entry + 6) != WIN_CERT_PKCS_SIGNED) continue;
		*signature = entry + WIN_CERT_HEADER_BYTES;
		*signature_bytes = length - WIN_CERT_HEADER_BYTES;
		return 1;
	}
	return 0;
}
