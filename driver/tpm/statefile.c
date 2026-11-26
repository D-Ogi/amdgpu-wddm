/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TPM state file. The layout and the contract are in statefile.h. No C library: it builds for the stage and
 * the user-mode core alike. */
#include "statefile.h"

#define RSMB_HEADER	8u
#define LABEL		"BC-250 TPM state key, version 1"
#define LABEL_BYTES	(sizeof(LABEL) - 1u)
#define MAX_SERIAL	64u

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

static void fill(unsigned char *p, unsigned char v, size_t bytes)
{
	volatile unsigned char *q = p;	/* the plaintext and the key must really go */
	size_t i;
	for (i = 0; i < bytes; i++) q[i] = v;
}

/* The end of the structure at p (the byte after its double zero), or 0 when it does not end inside the table. */
static size_t structure_end(const unsigned char *t, size_t bytes, size_t p)
{
	size_t s;
	if (p + 4 > bytes || t[p + 1] < 4 || p + t[p + 1] > bytes) return 0;
	for (s = p + t[p + 1]; s + 1 < bytes; s++)
		if (t[s] == 0 && t[s + 1] == 0) return s + 2;
	return 0;
}

/* String number n (1 is the first) of the structure at p, with its length. NULL for 0 or a missing string. */
static const unsigned char *structure_string(const unsigned char *t, size_t end, size_t p, unsigned n, size_t *len)
{
	size_t s = p + t[p + 1];
	unsigned k;
	if (n == 0) return NULL;
	for (k = 1; s < end && t[s] != 0; k++) {
		size_t e = s;
		while (e < end && t[e] != 0) e++;
		if (k == n) { *len = e - s; return t + s; }
		s = e + 1;
	}
	return NULL;
}

int tpm_statefile_board_key(const unsigned char *rsmb, size_t bytes, tpm_statefile_sha256 sha256, unsigned char key[32])
{
	unsigned char in[LABEL_BYTES + 16u + 2u + MAX_SERIAL];
	const unsigned char *t, *serial = NULL;
	size_t n, p = 0, serial_len = 0, used;
	int have_uuid = 0;
	unsigned i;

	if (rsmb == NULL || sha256 == NULL || key == NULL || bytes < RSMB_HEADER) return 0;
	t = rsmb + RSMB_HEADER;
	n = bytes - RSMB_HEADER;
	if (get(rsmb + 4, 4) < n) n = (size_t)get(rsmb + 4, 4);
	for (i = 0; i < LABEL_BYTES; i++) in[i] = (unsigned char)LABEL[i];
	while (p < n) {
		size_t end = structure_end(t, n, p);
		if (end == 0) break;
		if (t[p] == 1 && t[p + 1] >= 0x19 && !have_uuid) {
			for (i = 0; i < 16; i++) in[LABEL_BYTES + i] = t[p + 8 + i];
			have_uuid = 1;
		}
		if (t[p] == 2 && t[p + 1] >= 8 && serial == NULL) {
			serial = structure_string(t, end, p, t[p + 7], &serial_len);
			if (serial_len > MAX_SERIAL) serial_len = MAX_SERIAL;
		}
		if (t[p] == 127) break;
		p = end;
	}
	if (!have_uuid) { fill(in, 0, sizeof(in)); return 0; }
	if (serial == NULL) serial_len = 0;
	put(in + LABEL_BYTES + 16, serial_len, 2);
	for (i = 0; i < serial_len; i++) in[LABEL_BYTES + 18 + i] = serial[i];
	used = LABEL_BYTES + 18u + serial_len;
	sha256(in, used, key);
	fill(in, 0, sizeof(in));
	return 1;
}

size_t tpm_statefile_write(unsigned char *out, size_t capacity, unsigned long long sequence, const unsigned char nonce[12],
			   const unsigned char key[32], const unsigned char *blob, size_t blob_bytes, tpm_statefile_gcm gcm)
{
	size_t total = TPM_STATEFILE_HEADER_BYTES + blob_bytes + TPM_STATEFILE_TAG_BYTES;
	unsigned i;

	if (out == NULL || nonce == NULL || key == NULL || blob == NULL || gcm == NULL || sequence == 0 ||
	    blob_bytes == 0 || blob_bytes > TPM_STATEFILE_MAX_BLOB || total > capacity)
		return 0;
	fill(out, 0, TPM_STATEFILE_HEADER_BYTES);
	put(out, TPM_STATEFILE_MAGIC, 4);
	put(out + 4, TPM_STATEFILE_VERSION, 2);
	put(out + 6, TPM_STATEFILE_HEADER_BYTES, 2);
	put(out + 8, sequence, 8);
	put(out + 16, blob_bytes, 4);
	for (i = 0; i < 12; i++) out[24 + i] = nonce[i];
	if (!gcm(1, key, nonce, out, TPM_STATEFILE_HEADER_BYTES, blob, blob_bytes, out + TPM_STATEFILE_HEADER_BYTES,
		 out + TPM_STATEFILE_HEADER_BYTES + blob_bytes)) {
		fill(out, 0, total);
		return 0;
	}
	return total;
}

enum tpm_statefile_result tpm_statefile_read(const unsigned char *file, size_t bytes, const unsigned char key[32],
					     tpm_statefile_gcm gcm, unsigned char *blob, size_t capacity,
					     size_t *blob_bytes, unsigned long long *sequence)
{
	unsigned long long seq, n;
	unsigned char tag[TPM_STATEFILE_TAG_BYTES];
	unsigned i;

	if (file == NULL || bytes < TPM_STATEFILE_HEADER_BYTES || get(file, 4) != TPM_STATEFILE_MAGIC) return TPM_STATEFILE_NONE;
	seq = get(file + 8, 8);
	n = get(file + 16, 4);
	if (get(file + 4, 2) != TPM_STATEFILE_VERSION || get(file + 6, 2) != TPM_STATEFILE_HEADER_BYTES || seq == 0 ||
	    n == 0 || n > TPM_STATEFILE_MAX_BLOB || TPM_STATEFILE_HEADER_BYTES + n + TPM_STATEFILE_TAG_BYTES != bytes ||
	    key == NULL || gcm == NULL)
		return TPM_STATEFILE_BAD_FRAME;
	if (blob == NULL || n > capacity) return TPM_STATEFILE_SMALL;
	for (i = 0; i < TPM_STATEFILE_TAG_BYTES; i++) tag[i] = file[TPM_STATEFILE_HEADER_BYTES + n + i];
	if (!gcm(0, key, file + 24, file, TPM_STATEFILE_HEADER_BYTES, file + TPM_STATEFILE_HEADER_BYTES, (size_t)n, blob, tag)) {
		fill(blob, 0, (size_t)n);
		return TPM_STATEFILE_BAD_TAG;
	}
	if (blob_bytes != NULL) *blob_bytes = (size_t)n;
	if (sequence != NULL) *sequence = seq;
	return TPM_STATEFILE_OK;
}

int tpm_statefile_choose(enum tpm_statefile_result a, unsigned long long sequence_a, enum tpm_statefile_result b,
			 unsigned long long sequence_b)
{
	if (a == TPM_STATEFILE_OK && b == TPM_STATEFILE_OK) return sequence_b > sequence_a ? 1 : 0;
	if (a == TPM_STATEFILE_OK) return 0;
	if (b == TPM_STATEFILE_OK) return 1;
	return -1;
}

int tpm_statefile_next(int chosen, unsigned long long sequence_a, unsigned long long sequence_b,
		       unsigned long long *sequence)
{
	if (chosen < 0) { *sequence = 1; return 0; }
	*sequence = (chosen == 0 ? sequence_a : sequence_b) + 1u;
	return chosen == 0 ? 1 : 0;
}
