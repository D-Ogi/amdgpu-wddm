/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TPM 2.0 commands of the pre-OS TPM. The contract and the sources are in tl_tpm2.h. */
#include <string.h>
#include "tl_tpm2.h"

/* edk2 MdePkg/Include/IndustryStandard/Tpm20.h */
#define TPM_ST_NO_SESSIONS	0x8001u
#define TPM_ST_SESSIONS		0x8002u
#define TPM_CC_PCR_Allocate	0x0000012Bu
#define TPM_CC_Startup		0x00000144u
#define TPM_CC_Shutdown		0x00000145u
#define TPM_CC_GetCapability	0x0000017Au
#define TPM_CC_PCR_Read		0x0000017Eu
#define TPM_CC_PCR_Extend	0x00000182u
#define TPM_CAP_PCRS		0x00000005u
#define TPM_CAP_TPM_PROPERTIES	0x00000006u
#define TPM_RS_PW		0x40000009u
#define TPM_RH_PLATFORM		0x4000000Cu
#define TPM_ALG_SHA256		0x000Bu
#define PCR_SELECT_BYTES	3u		/* 24 PCRs */
#define HEADER_BYTES		10u		/* tag, size, command or response code */
/* An authorization area with one empty password session: sessionHandle, nonce (size 0), attributes, hmac (size 0). */
#define PW_SESSION_BYTES	9u

struct out {
	unsigned char *p;
	size_t cap, n;
	int bad;
};

static void put(struct out *o, unsigned long long v, unsigned bytes)
{
	unsigned i;
	if (o->bad || o->cap - o->n < bytes) {
		o->bad = 1;
		return;
	}
	for (i = 0; i < bytes; i++) o->p[o->n + i] = (unsigned char)(v >> (8u * (bytes - 1u - i)));
	o->n += bytes;
}

static void put_bytes(struct out *o, const unsigned char *b, size_t bytes)
{
	if (o->bad || o->cap - o->n < bytes) {
		o->bad = 1;
		return;
	}
	memcpy(o->p + o->n, b, bytes);
	o->n += bytes;
}

static void begin(struct out *o, unsigned char *p, size_t cap, unsigned tag, unsigned long code)
{
	o->p = p;
	o->cap = cap;
	o->n = 0;
	o->bad = 0;
	put(o, tag, 2);
	put(o, 0, 4);		/* the size, written at the end */
	put(o, code, 4);
}

static size_t end(struct out *o)
{
	unsigned i;
	if (o->bad) return 0;
	for (i = 0; i < 4; i++) o->p[2 + i] = (unsigned char)(o->n >> (8u * (3u - i)));
	return o->n;
}

static void pw_session(struct out *o)
{
	put(o, PW_SESSION_BYTES, 4);	/* authorizationSize */
	put(o, TPM_RS_PW, 4);
	put(o, 0, 2);			/* nonceCaller: empty */
	put(o, 0, 1);			/* sessionAttributes */
	put(o, 0, 2);			/* hmac: the empty password */
}

static unsigned long get(const unsigned char *p, unsigned bytes)
{
	unsigned long v = 0;
	unsigned i;
	for (i = 0; i < bytes; i++) v = (v << 8) | p[i];
	return v;
}

unsigned tl_tpm2_rc(const unsigned char *rsp, size_t bytes)
{
	if (rsp == NULL || bytes < HEADER_BYTES || get(rsp + 2, 4) != bytes) return TL_TPM2_MALFORMED;
	return (unsigned)get(rsp + 6, 4);
}

size_t tl_tpm2_startup(unsigned char *out, size_t cap, unsigned startup_type)
{
	struct out o;
	begin(&o, out, cap, TPM_ST_NO_SESSIONS, TPM_CC_Startup);
	put(&o, startup_type, 2);
	return end(&o);
}

size_t tl_tpm2_shutdown(unsigned char *out, size_t cap, unsigned shutdown_type)
{
	struct out o;
	begin(&o, out, cap, TPM_ST_NO_SESSIONS, TPM_CC_Shutdown);
	put(&o, shutdown_type, 2);
	return end(&o);
}

size_t tl_tpm2_pcr_extend(unsigned char *out, size_t cap, unsigned pcr, const unsigned char digest[TL_TPM2_SHA256_BYTES])
{
	struct out o;
	if (pcr >= TL_TPM2_PCRS) return 0;
	begin(&o, out, cap, TPM_ST_SESSIONS, TPM_CC_PCR_Extend);
	put(&o, pcr, 4);		/* pcrHandle: a PCR handle is its index */
	pw_session(&o);
	put(&o, 1, 4);			/* TPML_DIGEST_VALUES.count */
	put(&o, TPM_ALG_SHA256, 2);
	put_bytes(&o, digest, TL_TPM2_SHA256_BYTES);
	return end(&o);
}

static void sha256_selection(struct out *o, unsigned first, unsigned count)
{
	unsigned char select[PCR_SELECT_BYTES] = {0, 0, 0};
	unsigned i;
	for (i = first; i < first + count && i < TL_TPM2_PCRS; i++) select[i / 8u] |= (unsigned char)(1u << (i % 8u));
	put(o, TPM_ALG_SHA256, 2);
	put(o, PCR_SELECT_BYTES, 1);
	put_bytes(o, select, PCR_SELECT_BYTES);
}

size_t tl_tpm2_pcr_read(unsigned char *out, size_t cap, unsigned first, unsigned count)
{
	struct out o;
	if (count == 0 || count > 8u || first >= TL_TPM2_PCRS || count > TL_TPM2_PCRS - first) return 0;
	begin(&o, out, cap, TPM_ST_NO_SESSIONS, TPM_CC_PCR_Read);
	put(&o, 1, 4);			/* TPML_PCR_SELECTION.count */
	sha256_selection(&o, first, count);
	return end(&o);
}

unsigned tl_tpm2_pcr_read_answer(const unsigned char *rsp, size_t bytes, unsigned first,
				 unsigned char pcrs[TL_TPM2_PCRS][TL_TPM2_SHA256_BYTES], unsigned *got)
{
	unsigned rc = tl_tpm2_rc(rsp, bytes);
	size_t at = HEADER_BYTES + 4u;	/* after pcrUpdateCounter */
	unsigned long selections, digests, i, k;
	unsigned select_bytes;
	unsigned char selected[PCR_SELECT_BYTES] = {0, 0, 0};
	unsigned pcr = 0;

	*got = 0;
	if (rc != 0) return rc;
	if (bytes < at + 4u) return TL_TPM2_MALFORMED;
	selections = get(rsp + at, 4);
	at += 4;
	for (i = 0; i < selections; i++) {
		if (bytes < at + 3u) return TL_TPM2_MALFORMED;
		select_bytes = rsp[at + 2];
		if (bytes < at + 3u + select_bytes) return TL_TPM2_MALFORMED;
		if (get(rsp + at, 2) == TPM_ALG_SHA256)
			for (k = 0; k < select_bytes && k < PCR_SELECT_BYTES; k++) selected[k] = rsp[at + 3 + k];
		at += 3u + select_bytes;
	}
	if (bytes < at + 4u) return TL_TPM2_MALFORMED;
	digests = get(rsp + at, 4);
	at += 4;
	/* The digests come in the order of the selected PCRs (TPM 2.0 Part 3, TPM2_PCR_Read). */
	for (i = 0; i < digests; i++) {
		unsigned long size;
		while (pcr < TL_TPM2_PCRS && !(selected[pcr / 8u] & (1u << (pcr % 8u)))) pcr++;
		if (pcr >= TL_TPM2_PCRS || pcr < first || bytes < at + 2u) return TL_TPM2_MALFORMED;
		size = get(rsp + at, 2);
		if (size != TL_TPM2_SHA256_BYTES || bytes < at + 2u + size) return TL_TPM2_MALFORMED;
		memcpy(pcrs[pcr], rsp + at + 2, TL_TPM2_SHA256_BYTES);
		at += 2u + size;
		pcr++;
		(*got)++;
	}
	return 0;
}

size_t tl_tpm2_get_pcrs(unsigned char *out, size_t cap)
{
	struct out o;
	begin(&o, out, cap, TPM_ST_NO_SESSIONS, TPM_CC_GetCapability);
	put(&o, TPM_CAP_PCRS, 4);
	put(&o, 0, 4);			/* property */
	put(&o, 1, 4);			/* propertyCount */
	return end(&o);
}

unsigned tl_tpm2_get_pcrs_answer(const unsigned char *rsp, size_t bytes, int *sha256_only, int *sha256_full)
{
	unsigned rc = tl_tpm2_rc(rsp, bytes);
	size_t at = HEADER_BYTES + 1u;	/* after moreData */
	unsigned long count, i, k;
	int other = 0, full = 0, sha256 = 0;

	*sha256_only = 0;
	*sha256_full = 0;
	if (rc != 0) return rc;
	if (bytes < at + 8u || get(rsp + at, 4) != TPM_CAP_PCRS) return TL_TPM2_MALFORMED;
	at += 4;
	count = get(rsp + at, 4);
	at += 4;
	for (i = 0; i < count; i++) {
		unsigned select_bytes, alg;
		int any = 0, all = 1;
		if (bytes < at + 3u) return TL_TPM2_MALFORMED;
		alg = (unsigned)get(rsp + at, 2);
		select_bytes = rsp[at + 2];
		if (bytes < at + 3u + select_bytes) return TL_TPM2_MALFORMED;
		for (k = 0; k < PCR_SELECT_BYTES; k++) {
			unsigned char b = k < select_bytes ? rsp[at + 3 + k] : 0;
			if (b) any = 1;
			if (b != 0xFF) all = 0;
		}
		for (k = PCR_SELECT_BYTES; k < select_bytes; k++)
			if (rsp[at + 3 + k]) any = 1;
		if (alg == TPM_ALG_SHA256) {
			sha256 = any;
			full = all;
		} else if (any) {
			other = 1;
		}
		at += 3u + select_bytes;
	}
	*sha256_only = sha256 && !other;
	*sha256_full = full;
	return 0;
}

unsigned tl_tpm2_get_pcrs_banks(const unsigned char *rsp, size_t bytes, unsigned *banks, unsigned capacity, unsigned *count)
{
	unsigned rc = tl_tpm2_rc(rsp, bytes);
	size_t at = HEADER_BYTES + 1u;
	unsigned long n, i;
	*count = 0;
	if (rc != 0) return rc;
	if (bytes < at + 8u || get(rsp + at, 4) != TPM_CAP_PCRS) return TL_TPM2_MALFORMED;
	n = get(rsp + at + 4, 4);
	at += 8;
	for (i = 0; i < n; i++) {
		unsigned select_bytes;
		if (bytes < at + 3u) return TL_TPM2_MALFORMED;
		select_bytes = rsp[at + 2];
		if (bytes < at + 3u + select_bytes) return TL_TPM2_MALFORMED;
		if (*count == capacity) return TL_TPM2_MALFORMED;
		banks[(*count)++] = (unsigned)get(rsp + at, 2);
		at += 3u + select_bytes;
	}
	return 0;
}

size_t tl_tpm2_get_property(unsigned char *out, size_t cap, unsigned long property)
{
	struct out o;
	begin(&o, out, cap, TPM_ST_NO_SESSIONS, TPM_CC_GetCapability);
	put(&o, TPM_CAP_TPM_PROPERTIES, 4);
	put(&o, property, 4);
	put(&o, 1, 4);
	return end(&o);
}

unsigned tl_tpm2_get_property_answer(const unsigned char *rsp, size_t bytes, unsigned long property, unsigned long *value)
{
	unsigned rc = tl_tpm2_rc(rsp, bytes);
	size_t at = HEADER_BYTES + 1u;	/* after moreData */
	unsigned long count, i;
	*value = 0;
	if (rc != 0) return rc;
	if (bytes < at + 8u || get(rsp + at, 4) != TPM_CAP_TPM_PROPERTIES) return TL_TPM2_MALFORMED;
	count = get(rsp + at + 4, 4);
	at += 8;
	for (i = 0; i < count; i++, at += 8) {
		if (bytes < at + 8u) return TL_TPM2_MALFORMED;
		if (get(rsp + at, 4) == property) {
			*value = get(rsp + at + 4, 4);
			return 0;
		}
	}
	return TL_TPM2_MALFORMED;	/* the TPM did not report it */
}

size_t tl_tpm2_pcr_allocate_sha256(unsigned char *out, size_t cap, const unsigned *other_banks, unsigned other_count)
{
	static const unsigned char none[PCR_SELECT_BYTES] = {0, 0, 0};
	struct out o;
	unsigned i;
	begin(&o, out, cap, TPM_ST_SESSIONS, TPM_CC_PCR_Allocate);
	put(&o, TPM_RH_PLATFORM, 4);
	pw_session(&o);
	put(&o, 1u + other_count, 4);	/* TPML_PCR_SELECTION.count */
	sha256_selection(&o, 0, TL_TPM2_PCRS);
	for (i = 0; i < other_count; i++) {
		put(&o, other_banks[i], 2);
		put(&o, PCR_SELECT_BYTES, 1);
		put_bytes(&o, none, PCR_SELECT_BYTES);
	}
	return end(&o);
}

unsigned tl_tpm2_pcr_allocate_answer(const unsigned char *rsp, size_t bytes, int *allocation_success)
{
	unsigned rc = tl_tpm2_rc(rsp, bytes);
	/* A command with sessions answers with parameterSize (UINT32) first, then allocationSuccess (TPMI_YES_NO). */
	*allocation_success = 0;
	if (rc != 0) return rc;
	if (bytes < HEADER_BYTES + 5u) return TL_TPM2_MALFORMED;
	*allocation_success = rsp[HEADER_BYTES + 4u] != 0;
	return 0;
}
