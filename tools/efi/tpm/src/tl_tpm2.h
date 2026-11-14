/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM 2.0 commands that the pre-OS TPM sends itself, and the parse of their answers. The byte layout is that of
 * TPM 2.0 Library Part 3 (Commands), with the constants of edk2 MdePkg/Include/IndustryStandard/Tpm20.h (fetched
 * 2026-11-14); no code is copied. All values on the wire are big-endian.
 *
 * Pure code. Each builder writes into out and returns the command size, 0 when out is too small. Each parser
 * returns the TPM response code (0 = success) or TL_TPM2_MALFORMED.
 */
#ifndef TL_TPM2_H
#define TL_TPM2_H
#include <stddef.h>

#define TL_TPM2_MALFORMED	0xFFFFFFFFu
#define TL_TPM2_SHA256_BYTES	32u
#define TL_TPM2_PCRS		24u

#define TL_TPM_SU_CLEAR		0x0000u
#define TL_TPM_RC_INITIALIZE	0x0100u		/* TPM2_Startup when the TPM already runs */

size_t tl_tpm2_startup(unsigned char *out, size_t cap, unsigned startup_type);
size_t tl_tpm2_shutdown(unsigned char *out, size_t cap, unsigned shutdown_type);

/* TPM2_PCR_Extend of one PCR in the SHA-256 bank, with an empty password session for the platform-less PCR auth
 * (TPM_RS_PW). */
size_t tl_tpm2_pcr_extend(unsigned char *out, size_t cap, unsigned pcr, const unsigned char digest[TL_TPM2_SHA256_BYTES]);

/* TPM2_PCR_Read of up to 8 PCRs of the SHA-256 bank, first .. first + count - 1. */
size_t tl_tpm2_pcr_read(unsigned char *out, size_t cap, unsigned first, unsigned count);
/* Copies the digests into pcrs[first ..]; *got is the number read. */
unsigned tl_tpm2_pcr_read_answer(const unsigned char *rsp, size_t bytes, unsigned first,
				 unsigned char pcrs[TL_TPM2_PCRS][TL_TPM2_SHA256_BYTES], unsigned *got);

/* TPM2_GetCapability(TPM_CAP_PCRS): the allocated banks. */
size_t tl_tpm2_get_pcrs(unsigned char *out, size_t cap);
/* *sha256_only = 1 when SHA-256 is the only bank with selected PCRs; *sha256_full = 1 when SHA-256 selects all
 * 24 PCRs. */
unsigned tl_tpm2_get_pcrs_answer(const unsigned char *rsp, size_t bytes, int *sha256_only, int *sha256_full);

/* TPM2_PCR_Allocate with platform auth (empty password): SHA-256 with all 24 PCRs, every other bank of the list
 * with none. It takes effect at the next TPM reset (TPM 2.0 Part 3, section 22.5). */
size_t tl_tpm2_pcr_allocate_sha256(unsigned char *out, size_t cap, const unsigned *other_banks, unsigned other_count);
unsigned tl_tpm2_pcr_allocate_answer(const unsigned char *rsp, size_t bytes, int *allocation_success);

/* The response code of any answer: 0 = success. */
unsigned tl_tpm2_rc(const unsigned char *rsp, size_t bytes);

#endif
