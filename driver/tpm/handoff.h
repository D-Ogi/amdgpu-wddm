/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM handoff record: what the pre-OS TPM of the stage leaves for the Windows core at ExitBootServices
 * (docs/design/tpm-preos.md, "The handoff range"). Three users with one format: the stage writes it
 * (tools/efi/tpm), the filter checks the frame and copies it out of the reserved range (driver/tpm/filter), the core
 * checks the digest and takes the two states (driver/tpm/core).
 *
 * Layout, little-endian:
 *   0  magic "BCTH"            4
 *   4  version (1)             2
 *   6  header bytes (80)       2
 *   8  SHA-256                32   over the bytes from offset 40 to the end of the record
 *  40  record bytes            4   header and both blobs
 *  44  permanent bytes         4   the permanent state of libtpms, at offset 80
 *  48  volatile bytes          4   the volatile state of libtpms, after the permanent state
 *  52  flags                   4   TPM_HANDOFF_* below
 *  56  stage volume           16   the GPT partition GUID of the volume of the stage, where the state files are
 *  72  sequence                8   the sequence of the state file that the stage started from (0: a new TPM)
 *  80  the blobs
 *
 * Pure code without a C library: the SHA-256 comes from the caller.
 */
#ifndef TPM_HANDOFF_H
#define TPM_HANDOFF_H
#include <stddef.h>

#define TPM_HANDOFF_MAGIC		0x48544342u	/* "BCTH" */
#define TPM_HANDOFF_VERSION		1u
#define TPM_HANDOFF_HEADER_BYTES	80u
#define TPM_HANDOFF_DIGEST_FROM		40u
#define TPM_HANDOFF_MAX_BYTES		(1024u * 1024u)	/* the range the stage reserves is at most this */

#define TPM_HANDOFF_STARTED		0x1u	/* the TPM ran TPM2_Startup in the stage: the core must not send it */
#define TPM_HANDOFF_PERMANENT_CHANGED	0x2u	/* libtpms stored NV in the stage: the core writes the state file */

typedef void (*tpm_handoff_sha256)(const void *data, size_t bytes, unsigned char digest[32]);

struct tpm_handoff_view {
	const unsigned char *permanent, *volatile_state;
	unsigned permanent_bytes, volatile_bytes;
	unsigned flags;
	unsigned char volume[16];
	unsigned long long sequence;
	unsigned record_bytes;
};

/* Writes a record into out. Returns its size, 0 when it does not fit or a size is out of range. */
size_t tpm_handoff_write(unsigned char *out, size_t capacity, const unsigned char volume[16], unsigned long long sequence,
			 unsigned flags, const unsigned char *permanent, unsigned permanent_bytes,
			 const unsigned char *volatile_state, unsigned volatile_bytes, tpm_handoff_sha256 sha256);

enum tpm_handoff_result {
	TPM_HANDOFF_OK = 0,
	TPM_HANDOFF_NONE,	/* no magic: no record (a stage without the pre-OS TPM, or a cleared range) */
	TPM_HANDOFF_BAD_FRAME,	/* a version, header size or size that does not fit the range */
	TPM_HANDOFF_BAD_DIGEST	/* the frame is sound and the digest differs */
};

/* Checks the frame (magic, version, sizes within bytes). With sha256 NULL the digest is not checked: the filter
 * checks the frame only and the core checks the digest of its copy. */
enum tpm_handoff_result tpm_handoff_read(const unsigned char *record, size_t bytes, tpm_handoff_sha256 sha256,
					 struct tpm_handoff_view *view);

#endif
