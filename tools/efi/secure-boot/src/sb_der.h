/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A reader for DER (ITU-T X.690, the definite-length form of sections 8.1.2 and 8.1.3), for the two formats that
 * the Secure Boot module reads: X.509 certificates and PKCS #7 SignedData. It reads one tag-length-value at a time
 * from a span and never reads past the span. The indefinite length and the long tag form are refused.
 */
#ifndef SB_DER_H
#define SB_DER_H
#include <stddef.h>

struct sb_span {
	const unsigned char *p;
	size_t n;
};

struct sb_tlv {
	unsigned tag;			/* the identifier octet */
	struct sb_span value;		/* the contents octets */
	struct sb_span whole;		/* identifier, length and contents */
};

#define SB_DER_BOOLEAN		0x01u
#define SB_DER_INTEGER		0x02u
#define SB_DER_BIT_STRING	0x03u
#define SB_DER_OCTET_STRING	0x04u
#define SB_DER_NULL		0x05u
#define SB_DER_OID		0x06u
#define SB_DER_SEQUENCE		0x30u
#define SB_DER_SET		0x31u
#define SB_DER_CONTEXT(n)	(0xA0u | (n))	/* a constructed context-specific tag */

/* Reads the element at the front of in and moves in past it. 1 on success. */
int sb_der_next(struct sb_span *in, struct sb_tlv *out);
/* The same, and the tag must be the given one. */
int sb_der_expect(struct sb_span *in, unsigned tag, struct sb_tlv *out);
/* 1 when the two spans hold the same bytes. */
int sb_span_equal(const struct sb_span *a, const struct sb_span *b);
int sb_span_is(const struct sb_span *a, const unsigned char *bytes, size_t n);

#endif
