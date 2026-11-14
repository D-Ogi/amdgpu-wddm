/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The command writer and response reader of the engine test (tpm_cmd.h). */
#include <string.h>
#include "tpm_cmd.h"

static void put(struct tpm_cmd *c, unsigned v)
{
	if (c->n < sizeof(c->b)) c->b[c->n] = (unsigned char)v;
	else c->overflow = 1;
	c->n++;
}

void tpm_cmd_u8(struct tpm_cmd *c, unsigned v) { put(c, v); }

void tpm_cmd_u16(struct tpm_cmd *c, unsigned v)
{
	put(c, v >> 8);
	put(c, v);
}

void tpm_cmd_u32(struct tpm_cmd *c, unsigned v)
{
	put(c, v >> 24);
	put(c, v >> 16);
	put(c, v >> 8);
	put(c, v);
}

void tpm_cmd_bytes(struct tpm_cmd *c, const void *p, size_t n)
{
	const unsigned char *b = p;
	while (n--) put(c, *b++);
}

void tpm_cmd_b2b(struct tpm_cmd *c, const void *p, size_t n)
{
	tpm_cmd_u16(c, (unsigned)n);
	tpm_cmd_bytes(c, p, n);
}

void tpm_cmd_begin(struct tpm_cmd *c, unsigned tag, unsigned code)
{
	c->n = 0;
	c->overflow = 0;
	tpm_cmd_u16(c, tag);
	tpm_cmd_u32(c, 0);
	tpm_cmd_u32(c, code);
}

void tpm_cmd_password_session(struct tpm_cmd *c)
{
	tpm_cmd_u32(c, 9);		/* authorizationSize */
	tpm_cmd_u32(c, TPM_RS_PW);	/* sessionHandle */
	tpm_cmd_u16(c, 0);		/* nonce: empty */
	tpm_cmd_u8(c, 0);		/* sessionAttributes */
	tpm_cmd_u16(c, 0);		/* hmac: the empty password */
}

size_t tpm_cmd_end(struct tpm_cmd *c)
{
	if (c->overflow || c->n < 10) return 0;
	c->b[2] = (unsigned char)(c->n >> 24);
	c->b[3] = (unsigned char)(c->n >> 16);
	c->b[4] = (unsigned char)(c->n >> 8);
	c->b[5] = (unsigned char)c->n;
	return c->n;
}

static unsigned get(struct tpm_rsp *r)
{
	if (r->at >= r->n) {
		r->bad = 1;
		return 0;
	}
	return r->p[r->at++];
}

unsigned tpm_rsp_u16(struct tpm_rsp *r)
{
	unsigned v = get(r) << 8;
	return v | get(r);
}

unsigned tpm_rsp_u32(struct tpm_rsp *r)
{
	unsigned v = tpm_rsp_u16(r) << 16;
	return v | tpm_rsp_u16(r);
}

void tpm_rsp_skip(struct tpm_rsp *r, size_t n)
{
	if (n > r->n - r->at) {
		r->bad = 1;
		r->at = r->n;
	} else {
		r->at += n;
	}
}

unsigned tpm_rsp_begin(struct tpm_rsp *r, const unsigned char *p, size_t n)
{
	unsigned size;
	r->p = p;
	r->n = n;
	r->at = 0;
	r->bad = 0;
	if (n < 10) return 0xFFFFFFFFu;
	(void)tpm_rsp_u16(r);
	size = tpm_rsp_u32(r);
	if (size != n) return 0xFFFFFFFFu;
	return tpm_rsp_u32(r);
}

const unsigned char *tpm_rsp_b2b(struct tpm_rsp *r, size_t *n, const unsigned char **raw)
{
	const unsigned char *start = r->p + r->at, *data;
	*n = tpm_rsp_u16(r);
	data = r->p + r->at;
	tpm_rsp_skip(r, *n);
	if (raw) *raw = start;
	return r->bad ? NULL : data;
}
