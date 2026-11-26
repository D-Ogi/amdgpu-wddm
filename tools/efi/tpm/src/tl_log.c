/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TCG event log. The contract and the sources are in tl_log.h. */
#include <string.h>
#include "efl_sha256.h"
#include "tl_log.h"

#define PFP_ERRATA_105	105u	/* specErrata: TCG_EfiSpecIDEventStruct_SPEC_ERRATA_TPM2_REV_105 of UefiTcgPlatform.h */

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
	put16(p, (unsigned)(v & 0xFFFFu));
	put16(p + 2, (unsigned)((v >> 16) & 0xFFFFu));
}

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static unsigned long get32(const unsigned char *p) { return (unsigned long)get16(p) | ((unsigned long)get16(p + 2) << 16); }

static void put64(unsigned char *p, unsigned long long v)
{
	put32(p, (unsigned long)(v & 0xFFFFFFFFu));
	put32(p + 4, (unsigned long)(v >> 32));
}

static unsigned long long get64(const unsigned char *p)
{
	return (unsigned long long)get32(p) | ((unsigned long long)get32(p + 4) << 32);
}

static size_t write_event2(unsigned char *at, unsigned pcr, unsigned type, const unsigned char *digest,
			   const void *data, size_t bytes)
{
	put32(at, pcr);
	put32(at + 4, type);
	put32(at + 8, 1u);			/* TPML_DIGEST_VALUES.count */
	put16(at + 12, TL_ALG_SHA256);
	if (digest != NULL)
		memcpy(at + 14, digest, TL_SHA256_BYTES);
	else
		memset(at + 14, 0, TL_SHA256_BYTES);
	put32(at + 14 + TL_SHA256_BYTES, (unsigned long)bytes);
	if (bytes) memcpy(at + TL_EVENT2_HEADER_BYTES, data, bytes);
	return TL_EVENT2_HEADER_BYTES + bytes;
}

int tl_log_init(struct tl_log *log, unsigned char *area, size_t capacity, unsigned char *final, size_t final_capacity)
{
	static const char signature[16] = "Spec ID Event03";
	unsigned char *e;

	memset(log, 0, sizeof(*log));
	if (area == NULL || final == NULL || capacity < TL_SPEC_ID_ENTRY_BYTES || final_capacity < TL_FINAL_HEADER_BYTES)
		return 0;
	memset(area, 0xFF, capacity);
	memset(final, 0xFF, final_capacity);
	log->area = area;
	log->capacity = capacity;
	log->final = final;
	log->final_capacity = final_capacity;

	/* TCG_PCR_EVENT: PCR 0, EV_NO_ACTION, a zero SHA-1 sized digest, the size of the Spec ID event. */
	put32(area, 0);
	put32(area + 4, TL_EV_NO_ACTION);
	memset(area + 8, 0, 20);
	put32(area + 28, TL_SPEC_ID_EVENT_BYTES);
	e = area + 32;
	memcpy(e, signature, 16);
	put32(e + 16, 0);			/* platformClass: client */
	e[20] = 0;				/* specVersionMinor */
	e[21] = 2;				/* specVersionMajor: TPM 2.0 */
	e[22] = (unsigned char)PFP_ERRATA_105;
	e[23] = 2;				/* uintnSize: in UINT32 units, 2 for a 64-bit UINTN */
	put32(e + 24, 1u);			/* numberOfAlgorithms */
	put16(e + 28, TL_ALG_SHA256);
	put16(e + 30, TL_SHA256_BYTES);
	e[32] = 0;				/* vendorInfoSize */
	log->used = TL_SPEC_ID_ENTRY_BYTES;
	log->last = 0;

	put64(final, 1u);			/* EFI_TCG2_FINAL_EVENTS_TABLE_VERSION */
	put64(final + 8, 0u);
	log->final_used = TL_FINAL_HEADER_BYTES;
	return 1;
}

int tl_log_add(struct tl_log *log, unsigned pcr, unsigned type, const unsigned char *digest, const void *data,
	       size_t bytes)
{
	size_t size = TL_EVENT2_HEADER_BYTES + bytes;
	int complete = 1;

	if (bytes > 0xFFFFFFFFu || (bytes && data == NULL)) return 0;
	if (!log->truncated && size <= log->capacity - log->used) {
		log->last = log->used;
		log->used += write_event2(log->area + log->used, pcr, type, digest, data, bytes);
	} else {
		log->truncated = 1;
		complete = 0;
	}
	if (log->final_on) {
		if (!log->final_truncated && size <= log->final_capacity - log->final_used) {
			log->final_used += write_event2(log->final + log->final_used, pcr, type, digest, data, bytes);
			put64(log->final + 8, get64(log->final + 8) + 1u);
		} else {
			log->final_truncated = 1;
			complete = 0;
		}
	}
	return complete;
}

void tl_log_final_start(struct tl_log *log) { log->final_on = 1; }

size_t tl_log_last(const struct tl_log *log) { return log->last; }

/* ---- reading ----------------------------------------------------------------------------------------- */

/* The Spec ID entry: TCG_PCR_EVENT of PCR 0, EV_NO_ACTION, then TCG_EfiSpecIDEventStruct with its table of
 * algorithms and digest sizes (TCG EFI Protocol Specification, section 5.3). */
static int read_spec_id(const unsigned char *bytes, size_t rest, struct tl_reader *reader, struct tl_event *event)
{
	const unsigned char *e = bytes + 32;
	unsigned long n, algs, i;

	if (rest < TL_SPEC_ID_ENTRY_BYTES || get32(bytes) != 0 || get32(bytes + 4) != TL_EV_NO_ACTION) return -1;
	n = get32(bytes + 28);
	if (n < TL_SPEC_ID_EVENT_BYTES || n > rest - 32 || memcmp(e, "Spec ID Event03", 16) != 0 || e[21] != 2) return -1;
	algs = get32(e + 24);
	if (algs == 0 || algs > TL_READER_MAX_ALGS || 28u + algs * 4u + 1u > n) return -1;
	reader->algs = (unsigned)algs;
	for (i = 0; i < algs; i++) {
		reader->id[i] = get16(e + 28 + i * 4u);
		reader->size[i] = get16(e + 30 + i * 4u);
		if (reader->size[i] == 0 || reader->size[i] > 64u) return -1;
	}
	event->type = TL_EV_NO_ACTION;
	event->data = e;
	event->bytes = n;
	event->size = 32 + n;
	return 1;
}

int tl_log_next(const unsigned char *bytes, size_t length, size_t *cursor, struct tl_reader *reader,
		struct tl_event *event)
{
	size_t at = *cursor, rest, p;
	unsigned long count, i, n;

	memset(event, 0, sizeof(*event));
	if (at >= length) return 0;
	rest = length - at;
	if (at == 0) {
		int r = read_spec_id(bytes, rest, reader, event);
		if (r == 1) *cursor = event->size;
		return r;
	}
	if (reader->algs == 0) return -1;
	if (rest < 4 || get32(bytes + at) == 0xFFFFFFFFu) return 0;
	if (rest < 12) return -1;
	event->pcr = (unsigned)get32(bytes + at);
	event->type = (unsigned)get32(bytes + at + 4);
	count = get32(bytes + at + 8);
	if (count > reader->algs) return -1;
	p = 12;
	for (i = 0; i < count; i++) {
		unsigned id, k, size = 0;
		if (rest - p < 2) return -1;
		id = get16(bytes + at + p);
		for (k = 0; k < reader->algs; k++)
			if (reader->id[k] == id) size = reader->size[k];
		if (size == 0 || rest - p - 2 < size) return -1;	/* an algorithm that the Spec ID entry does not declare */
		if (id == TL_ALG_SHA256) event->digest = bytes + at + p + 2;
		p += 2u + size;
	}
	if (rest - p < 4) return -1;
	n = get32(bytes + at + p);
	p += 4;
	if (n > rest - p) return -1;
	event->digests = (unsigned)count;
	event->data = bytes + at + p;
	event->bytes = n;
	event->offset = at;
	event->size = p + n;
	*cursor = at + event->size;
	return 1;
}

int tl_log_replay(const unsigned char *bytes, size_t length, unsigned char pcrs[24][TL_SHA256_BYTES])
{
	struct tl_reader reader;
	struct tl_event event;
	size_t cursor = 0;
	int r, count = 0;

	memset(&reader, 0, sizeof(reader));
	memset(pcrs, 0, 24u * TL_SHA256_BYTES);
	/* PCR 17 to 22 are the dynamic root PCRs: a TPM2_Startup sets them to all ones, and only a dynamic launch
	 * resets them to zero (PC Client Platform TPM Profile, PCR attributes). This log has no dynamic launch. */
	memset(pcrs[17], 0xFF, 6u * TL_SHA256_BYTES);
	while ((r = tl_log_next(bytes, length, &cursor, &reader, &event)) == 1) {
		struct efl_sha256 s;
		count++;
		/* EV_NO_ACTION extends nothing (TCG PC Client PFP; edk2 TcgDxeHashLogExtendEvent). */
		if (event.type == TL_EV_NO_ACTION || event.digest == NULL || event.pcr >= 24u) continue;
		efl_sha256_init(&s);
		efl_sha256_update(&s, pcrs[event.pcr], TL_SHA256_BYTES);
		efl_sha256_update(&s, event.digest, TL_SHA256_BYTES);
		efl_sha256_final(&s, pcrs[event.pcr]);
	}
	return r < 0 ? -1 : count;
}
