/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The logic of EFI_TCG2_PROTOCOL (TCG EFI Protocol Specification, family 2.0, level 00 revision 00.13, section 6),
 * without the UEFI binding: tpm_module.c maps the protocol calls onto these functions. The parameter checks and the
 * 1.0 compatibility of GetCapability follow edk2 SecurityPkg/Tcg/Tcg2Dxe/Tcg2Dxe.c (Tcg2GetCapability,
 * Tcg2HashLogExtendEvent; fetched 2026-11-14); no code is copied.
 */
#ifndef TL_TCG2_H
#define TL_TCG2_H
#include <stddef.h>
#include "tl_measure.h"

/* EFI_TCG2_BOOT_SERVICE_CAPABILITY is not packed (section 6.4.4 of the specification): with natural alignment
 * its fields are at these offsets, 36 bytes in all. The 1.0 form ends after ManufacturerID. */
#define TL_CAP_BYTES		36u
#define TL_CAP_1_0_BYTES	28u

#define TL_HASH_ALG_SHA256	0x00000002u	/* EFI_TCG2_BOOT_HASH_ALG_SHA256 */
#define TL_LOG_FORMAT_TCG_2	0x00000002u	/* EFI_TCG2_EVENT_LOG_FORMAT_TCG_2 */
#define TL_EXTEND_ONLY		0x0000000000000001ull
#define TL_PE_COFF_IMAGE	0x0000000000000010ull
#define TL_MAX_PCR_INDEX	23u

/* The result of a call, mapped to an EFI_STATUS by the binding. */
enum tl_status {
	TL_OK = 0,
	TL_INVALID_PARAMETER,
	TL_BUFFER_TOO_SMALL,
	TL_DEVICE_ERROR,
	TL_VOLUME_FULL,
	TL_UNSUPPORTED
};

struct tl_capability {
	int present;			/* TPMPresentFlag */
	unsigned max_command, max_response;
	unsigned long manufacturer;	/* TPM_PT_MANUFACTURER, big-endian ASCII as the TPM gives it */
};

/* GetCapability into the caller's structure. Its first byte, Size, says how much the caller allocated; on
 * TL_BUFFER_TOO_SMALL only Size is written, with the size that is necessary. */
enum tl_status tl_tcg2_capability(const struct tl_capability *cap, unsigned char *out);

/* EFI_TCG2_EVENT: Size (UINT32), then EFI_TCG2_EVENT_HEADER (HeaderSize UINT32, HeaderVersion UINT16, PCRIndex
 * UINT32, EventType UINT32), then the event data, packed. */
#define TL_EVENT_FIXED_BYTES	18u

struct tl_tcg2_event {
	unsigned pcr, type;
	const unsigned char *data;
	size_t bytes;
};

/* Reads an EFI_TCG2_EVENT, with the checks of the specification. */
enum tl_status tl_tcg2_read_event(const unsigned char *event, struct tl_tcg2_event *out);

/* HashLogExtendEvent. data/bytes are DataToHash/DataToHashLen; with TL_PE_COFF_IMAGE the digest is the
 * Authenticode digest of a PE image in file layout. */
enum tl_status tl_tcg2_hash_log_extend(const struct tl_measurer *m, int present, unsigned long long flags,
				       const unsigned char *data, unsigned long long bytes,
				       const unsigned char *event);

#endif
