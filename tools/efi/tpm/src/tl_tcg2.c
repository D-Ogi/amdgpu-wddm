/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The logic of EFI_TCG2_PROTOCOL. The contract and the sources are in tl_tcg2.h. */
#include <string.h>
#include "efl_pe.h"
#include "efl_sha256.h"
#include "tl_tcg2.h"

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
	put16(p, (unsigned)(v & 0xFFFFu));
	put16(p + 2, (unsigned)(v >> 16));
}

static unsigned long get32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

enum tl_status tl_tcg2_capability(const struct tl_capability *cap, unsigned char *out)
{
	unsigned size;
	int one_zero;

	if (out == NULL) return TL_INVALID_PARAMETER;
	size = out[0];
	/* A caller of protocol version 1.0 allocates the 1.0 structure: it gets that, as from edk2. */
	one_zero = size < TL_CAP_BYTES;
	if (one_zero && size < TL_CAP_1_0_BYTES) {
		out[0] = (unsigned char)TL_CAP_BYTES;
		return TL_BUFFER_TOO_SMALL;
	}
	memset(out, 0, one_zero ? TL_CAP_1_0_BYTES : TL_CAP_BYTES);
	out[0] = (unsigned char)(one_zero ? TL_CAP_1_0_BYTES : TL_CAP_BYTES);
	out[1] = 1;				/* StructureVersion */
	out[2] = one_zero ? 0 : 1;
	out[3] = 1;				/* ProtocolVersion */
	out[4] = one_zero ? 0 : 1;
	put32(out + 8, TL_HASH_ALG_SHA256);	/* HashAlgorithmBitmap */
	put32(out + 12, TL_LOG_FORMAT_TCG_2);	/* SupportedEventLogs */
	out[16] = cap->present ? 1 : 0;		/* TPMPresentFlag */
	put16(out + 18, cap->max_command);
	put16(out + 20, cap->max_response);
	put32(out + 24, cap->manufacturer);
	if (one_zero) return TL_OK;
	put32(out + 28, 1u);			/* NumberOfPCRBanks */
	put32(out + 32, TL_HASH_ALG_SHA256);	/* ActivePcrBanks */
	return TL_OK;
}

enum tl_status tl_tcg2_read_event(const unsigned char *event, struct tl_tcg2_event *out)
{
	unsigned long size, header;
	memset(out, 0, sizeof(*out));
	if (event == NULL) return TL_INVALID_PARAMETER;
	size = get32(event);
	header = get32(event + 4);
	/* HeaderSize covers HeaderSize, HeaderVersion, PCRIndex and EventType; version 1 (section 6.6). */
	if (header < TL_EVENT_FIXED_BYTES - 4u || size < header + 4u || (event[8] | (event[9] << 8)) != 1u)
		return TL_INVALID_PARAMETER;
	out->pcr = (unsigned)get32(event + 10);
	out->type = (unsigned)get32(event + 14);
	out->data = event + 4u + header;
	out->bytes = size - 4u - header;
	if (out->type != TL_EV_NO_ACTION && out->pcr > TL_MAX_PCR_INDEX) return TL_INVALID_PARAMETER;
	return TL_OK;
}

enum tl_status tl_tcg2_hash_log_extend(const struct tl_measurer *m, int present, unsigned long long flags,
				       const unsigned char *data, unsigned long long bytes,
				       const unsigned char *event)
{
	struct tl_tcg2_event e;
	unsigned char digest[TL_SHA256_BYTES];
	enum tl_status status;

	if (event == NULL) return TL_INVALID_PARAMETER;
	status = tl_tcg2_read_event(event, &e);
	if (status != TL_OK) return status;
	if (e.type != TL_EV_NO_ACTION && data == NULL) return TL_INVALID_PARAMETER;
	if (!present) return TL_DEVICE_ERROR;
	if (bytes > (size_t)-1) return TL_INVALID_PARAMETER;

	if (e.type == TL_EV_NO_ACTION) {
		/* Logged with zero digests and never extended (TCG PC Client PFP; edk2 TcgDxeHashLogExtendEvent). An
		 * EV_NO_ACTION for a PCR above 23 would be an NV index extend, which this TPM does not offer. */
		if (e.pcr > TL_MAX_PCR_INDEX) return TL_UNSUPPORTED;
		if (flags & TL_EXTEND_ONLY) return TL_OK;
		return tl_log_add(m->log, e.pcr, e.type, NULL, e.data, e.bytes) ? TL_OK : TL_VOLUME_FULL;
	}
	if (flags & TL_PE_COFF_IMAGE) {
		struct efl_pe pe;
		if (!efl_pe_read(data, (size_t)bytes, &pe)) return TL_UNSUPPORTED;
		memcpy(digest, pe.digest, TL_SHA256_BYTES);
	} else {
		efl_sha256(data, (size_t)bytes, digest);
	}
	if (!m->extend(m->context, e.pcr, digest)) return TL_DEVICE_ERROR;
	if (flags & TL_EXTEND_ONLY) return TL_OK;
	return tl_log_add(m->log, e.pcr, e.type, digest, e.data, e.bytes) ? TL_OK : TL_VOLUME_FULL;
}
