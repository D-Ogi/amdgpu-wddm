/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TPM handoff record. The layout and the contract are in handoff.h. No C library: it builds for the stage,
 * the kernel filter and the user-mode core alike. */
#include "handoff.h"

static void put(unsigned char *p, unsigned long long v, unsigned bytes)
{
	unsigned i;
	for (i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8u * i));
}

static unsigned long long get(const unsigned char *p, unsigned bytes)
{
	unsigned long long v = 0;
	unsigned i;
	for (i = bytes; i-- > 0;) v = (v << 8) | p[i];
	return v;
}

static void copy(unsigned char *to, const unsigned char *from, size_t bytes)
{
	size_t i;
	for (i = 0; i < bytes; i++) to[i] = from[i];
}

static int same(const unsigned char *a, const unsigned char *b, size_t bytes)
{
	unsigned char d = 0;
	size_t i;
	for (i = 0; i < bytes; i++) d |= (unsigned char)(a[i] ^ b[i]);
	return d == 0;
}

size_t tpm_handoff_write(unsigned char *out, size_t capacity, const unsigned char volume[16], unsigned long long sequence,
			 unsigned flags, const unsigned char *permanent, unsigned permanent_bytes,
			 const unsigned char *volatile_state, unsigned volatile_bytes, tpm_handoff_sha256 sha256)
{
	size_t total = (size_t)TPM_HANDOFF_HEADER_BYTES + permanent_bytes + volatile_bytes;
	unsigned i;

	if (out == NULL || sha256 == NULL || permanent_bytes > TPM_HANDOFF_MAX_BYTES ||
	    volatile_bytes > TPM_HANDOFF_MAX_BYTES || total > TPM_HANDOFF_MAX_BYTES || total > capacity ||
	    (permanent_bytes && permanent == NULL) || (volatile_bytes && volatile_state == NULL))
		return 0;
	put(out, TPM_HANDOFF_MAGIC, 4);
	put(out + 4, TPM_HANDOFF_VERSION, 2);
	put(out + 6, TPM_HANDOFF_HEADER_BYTES, 2);
	put(out + 40, total, 4);
	put(out + 44, permanent_bytes, 4);
	put(out + 48, volatile_bytes, 4);
	put(out + 52, flags, 4);
	for (i = 0; i < 16; i++) out[56 + i] = volume != NULL ? volume[i] : 0;
	put(out + 72, sequence, 8);
	copy(out + TPM_HANDOFF_HEADER_BYTES, permanent, permanent_bytes);
	copy(out + TPM_HANDOFF_HEADER_BYTES + permanent_bytes, volatile_state, volatile_bytes);
	sha256(out + TPM_HANDOFF_DIGEST_FROM, total - TPM_HANDOFF_DIGEST_FROM, out + 8);
	return total;
}

enum tpm_handoff_result tpm_handoff_read(const unsigned char *record, size_t bytes, tpm_handoff_sha256 sha256,
					 struct tpm_handoff_view *view)
{
	unsigned long long total, permanent, volatile_bytes;
	unsigned i;

	if (record == NULL || bytes < TPM_HANDOFF_HEADER_BYTES || get(record, 4) != TPM_HANDOFF_MAGIC) return TPM_HANDOFF_NONE;
	total = get(record + 40, 4);
	permanent = get(record + 44, 4);
	volatile_bytes = get(record + 48, 4);
	if (get(record + 4, 2) != TPM_HANDOFF_VERSION || get(record + 6, 2) != TPM_HANDOFF_HEADER_BYTES ||
	    total > bytes || total > TPM_HANDOFF_MAX_BYTES || permanent > TPM_HANDOFF_MAX_BYTES ||
	    volatile_bytes > TPM_HANDOFF_MAX_BYTES || TPM_HANDOFF_HEADER_BYTES + permanent + volatile_bytes != total)
		return TPM_HANDOFF_BAD_FRAME;
	if (sha256 != NULL) {
		unsigned char digest[32];
		sha256(record + TPM_HANDOFF_DIGEST_FROM, (size_t)total - TPM_HANDOFF_DIGEST_FROM, digest);
		if (!same(digest, record + 8, 32)) return TPM_HANDOFF_BAD_DIGEST;
	}
	if (view != NULL) {
		view->record_bytes = (unsigned)total;
		view->permanent_bytes = (unsigned)permanent;
		view->volatile_bytes = (unsigned)volatile_bytes;
		view->permanent = record + TPM_HANDOFF_HEADER_BYTES;
		view->volatile_state = record + TPM_HANDOFF_HEADER_BYTES + permanent;
		view->flags = (unsigned)get(record + 52, 4);
		for (i = 0; i < 16; i++) view->volume[i] = record[56 + i];
		view->sequence = get(record + 72, 8);
	}
	return TPM_HANDOFF_OK;
}
