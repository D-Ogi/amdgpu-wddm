/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The DER reader (sb_der.h). */
#include <stddef.h>
#include <string.h>
#include "sb_der.h"

int sb_der_next(struct sb_span *in, struct sb_tlv *out)
{
	const unsigned char *p = in->p;
	size_t n = in->n, length, header;
	if (p == NULL || n < 2u) return 0;
	if ((p[0] & 0x1Fu) == 0x1Fu) return 0;		/* the long tag form */
	if (p[1] < 0x80u) {
		length = p[1];
		header = 2u;
	} else {
		unsigned count = p[1] & 0x7Fu, i;
		/* 0x80 is the indefinite form. More than 4 length octets is more than any file here. */
		if (count == 0 || count > 4u || n < 2u + count) return 0;
		length = 0;
		for (i = 0; i < count; i++) length = (length << 8) | p[2u + i];
		header = 2u + count;
	}
	if (length > n - header) return 0;
	out->tag = p[0];
	out->value.p = p + header;
	out->value.n = length;
	out->whole.p = p;
	out->whole.n = header + length;
	in->p += header + length;
	in->n -= header + length;
	return 1;
}

int sb_der_expect(struct sb_span *in, unsigned tag, struct sb_tlv *out)
{
	struct sb_span at = *in;
	if (!sb_der_next(&at, out) || out->tag != tag) return 0;
	*in = at;
	return 1;
}

int sb_span_equal(const struct sb_span *a, const struct sb_span *b)
{
	return a->n == b->n && (a->n == 0 || !memcmp(a->p, b->p, a->n));
}

int sb_span_is(const struct sb_span *a, const unsigned char *bytes, size_t n)
{
	return a->n == n && !memcmp(a->p, bytes, n);
}
