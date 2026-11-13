/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The signature database reader (sb_siglist.h). */
#include <stddef.h>
#include <string.h>
#include "sb_siglist.h"

/* EFI_SIGNATURE_LIST: SignatureType (a GUID), SignatureListSize, SignatureHeaderSize, SignatureSize, then the
 * header and the signatures. Each signature is an owner GUID and the data. */
#define GUID_BYTES		16u
#define LIST_SIZE_AT		16u
#define LIST_HEADER_SIZE_AT	20u
#define LIST_ENTRY_SIZE_AT	24u
#define LIST_HEADER_BYTES	28u
#define SHA256_BYTES		32u

/* The GUIDs in their stored byte order. */
static const unsigned char guid_x509[GUID_BYTES] = {		/* a5c059a1-94e4-4aa7-87b5-ab155c2bf072 */
	0xA1, 0x59, 0xC0, 0xA5, 0xE4, 0x94, 0xA7, 0x4A, 0x87, 0xB5, 0xAB, 0x15, 0x5C, 0x2B, 0xF0, 0x72
};
static const unsigned char guid_sha256[GUID_BYTES] = {		/* c1c41626-504c-4092-aca9-41f936934328 */
	0x26, 0x16, 0xC4, 0xC1, 0x4C, 0x50, 0x92, 0x40, 0xAC, 0xA9, 0x41, 0xF9, 0x36, 0x93, 0x43, 0x28
};

static size_t le32(const unsigned char *p)
{
	return (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
}

void sb_siglist_open(struct sb_sig_cursor *cursor, const unsigned char *database, size_t bytes)
{
	memset(cursor, 0, sizeof(*cursor));
	cursor->rest.p = database;
	cursor->rest.n = database ? bytes : 0;
}

int sb_siglist_next(struct sb_sig_cursor *cursor, enum sb_sig_type *type, struct sb_span *data)
{
	while (cursor->entries.n == 0) {
		const unsigned char *list = cursor->rest.p;
		size_t list_bytes, header_bytes, entry_bytes;
		if (cursor->rest.n == 0) return 0;
		if (cursor->rest.n < LIST_HEADER_BYTES) return -1;
		list_bytes = le32(list + LIST_SIZE_AT);
		header_bytes = le32(list + LIST_HEADER_SIZE_AT);
		entry_bytes = le32(list + LIST_ENTRY_SIZE_AT);
		if (list_bytes < LIST_HEADER_BYTES || list_bytes > cursor->rest.n ||
		    header_bytes > list_bytes - LIST_HEADER_BYTES || entry_bytes <= GUID_BYTES)
			return -1;
		/* The entries must fill the list exactly. */
		if ((list_bytes - LIST_HEADER_BYTES - header_bytes) % entry_bytes != 0) return -1;
		if (!memcmp(list, guid_x509, GUID_BYTES))
			cursor->type = SB_SIG_X509;
		else if (!memcmp(list, guid_sha256, GUID_BYTES)) {
			if (entry_bytes != GUID_BYTES + SHA256_BYTES) return -1;
			cursor->type = SB_SIG_SHA256;
		} else
			cursor->type = SB_SIG_OTHER;
		cursor->entries.p = list + LIST_HEADER_BYTES + header_bytes;
		cursor->entries.n = list_bytes - LIST_HEADER_BYTES - header_bytes;
		cursor->entry_bytes = entry_bytes;
		cursor->rest.p += list_bytes;
		cursor->rest.n -= list_bytes;
	}
	*type = cursor->type;
	data->p = cursor->entries.p + GUID_BYTES;
	data->n = cursor->entry_bytes - GUID_BYTES;
	cursor->entries.p += cursor->entry_bytes;
	cursor->entries.n -= cursor->entry_bytes;
	return 1;
}

int sb_siglist_count(const unsigned char *database, size_t bytes, struct sb_siglist_count *out)
{
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	int more;
	memset(out, 0, sizeof(*out));
	sb_siglist_open(&cursor, database, bytes);
	while ((more = sb_siglist_next(&cursor, &type, &data)) == 1) {
		if (type == SB_SIG_X509) out->x509++;
		else if (type == SB_SIG_SHA256) out->sha256++;
		else out->other++;
	}
	return more == 0;
}
