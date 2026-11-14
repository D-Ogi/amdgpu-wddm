/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM state file: the permanent state of libtpms, encrypted, on the EFI system partition
 * (docs/design/tpm-preos.md, "The state on the EFI system partition"). Two users with one format: the Windows core
 * writes it (driver/tpm/core), the stage reads it (tools/efi/tpm). Two copies, state-a.bin and state-b.bin; the
 * valid copy with the higher sequence is the state.
 *
 * Layout, little-endian:
 *   0  magic "BCTS"            4
 *   4  version (1)             2
 *   6  header bytes (48)       2
 *   8  sequence                8   higher is newer; 0 is not a valid sequence
 *  16  blob bytes              4   the permanent state of libtpms
 *  20  reserved (0)            4
 *  24  nonce                  12   one nonce for each write; never used twice with one key
 *  36  reserved (0)           12
 *  48  the encrypted blob
 *  48 + blob bytes: the tag   16
 *
 * AES-256-GCM over the blob, with the 48 header bytes as additional data: a changed byte anywhere in the file fails
 * the tag. The key comes from the board identity (tpm_statefile_board_key); a disk on another board does not open.
 *
 * Pure code without a C library: the SHA-256 and the AES-256-GCM come from the caller.
 */
#ifndef TPM_STATEFILE_H
#define TPM_STATEFILE_H
#include <stddef.h>

#define TPM_STATEFILE_MAGIC		0x53544342u	/* "BCTS" */
#define TPM_STATEFILE_VERSION		1u
#define TPM_STATEFILE_HEADER_BYTES	48u
#define TPM_STATEFILE_TAG_BYTES		16u
#define TPM_STATEFILE_MAX_BLOB		(512u * 1024u)	/* the permanent state of libtpms is about 30 KiB */
#define TPM_STATEFILE_MAX_BYTES		(TPM_STATEFILE_HEADER_BYTES + TPM_STATEFILE_MAX_BLOB + TPM_STATEFILE_TAG_BYTES)

typedef void (*tpm_statefile_sha256)(const void *data, size_t bytes, unsigned char digest[32]);

/* AES-256-GCM with a 12-byte nonce and a 16-byte tag. seal 1: encrypts n bytes of in to out and writes the tag.
 * seal 0: checks the tag first and decrypts only when it holds. Returns 1, or 0 on a failure or a wrong tag.
 * in and out are the same size and may be the same buffer. */
typedef int (*tpm_statefile_gcm)(int seal, const unsigned char key[32], const unsigned char nonce[12],
				 const unsigned char *aad, size_t aad_bytes, const unsigned char *in, size_t n,
				 unsigned char *out, unsigned char tag[16]);

/* The key from the board identity: SHA-256 over a fixed label, the SMBIOS system UUID (type 1) and the baseboard
 * serial (type 2, empty when the board has none). table is the RSMB form (efl_smbios.h): 8 header bytes, then the
 * structures. Returns 1, or 0 when the table has no type 1 structure with a UUID. The identity is a secret of the
 * board in the rules of this project: no caller prints it, and the key stays in memory. */
int tpm_statefile_board_key(const unsigned char *rsmb, size_t bytes, tpm_statefile_sha256 sha256, unsigned char key[32]);

/* Writes a state file into out. Returns its size, 0 when it does not fit, the blob is empty or too large, the
 * sequence is 0, or the encryption fails. */
size_t tpm_statefile_write(unsigned char *out, size_t capacity, unsigned long long sequence, const unsigned char nonce[12],
			   const unsigned char key[32], const unsigned char *blob, size_t blob_bytes, tpm_statefile_gcm gcm);

enum tpm_statefile_result {
	TPM_STATEFILE_OK = 0,
	TPM_STATEFILE_NONE,		/* no magic: not a state file (an empty or a foreign file) */
	TPM_STATEFILE_BAD_FRAME,	/* a version, header size, sequence or size that does not fit */
	TPM_STATEFILE_BAD_TAG,		/* the frame is sound and the tag fails: another key, or a changed byte */
	TPM_STATEFILE_SMALL		/* the blob does not fit in the buffer of the caller */
};

/* Checks and decrypts a state file into blob. On TPM_STATEFILE_OK, *blob_bytes and *sequence are set. On any other
 * result the buffer holds no part of the plaintext. */
enum tpm_statefile_result tpm_statefile_read(const unsigned char *file, size_t bytes, const unsigned char key[32],
					     tpm_statefile_gcm gcm, unsigned char *blob, size_t capacity,
					     size_t *blob_bytes, unsigned long long *sequence);

/* Which copy is the state: 0 for A, 1 for B, -1 for none. A copy counts when its result is TPM_STATEFILE_OK. With
 * two valid copies the higher sequence wins; the same sequence twice takes A. */
int tpm_statefile_choose(enum tpm_statefile_result a, unsigned long long sequence_a, enum tpm_statefile_result b,
			 unsigned long long sequence_b);

/* Where the next write goes: the copy that is not the state, with the sequence of the state + 1, so that a write
 * that a power cut breaks leaves the state as it was. chosen is the result of tpm_statefile_choose; with -1 (no
 * state) the write goes to A with sequence 1. Returns 0 for A, 1 for B. */
int tpm_statefile_next(int chosen, unsigned long long sequence_a, unsigned long long sequence_b,
		       unsigned long long *sequence);

#endif
