/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * SHA-256 (FIPS 180-4, section 6.2), written from the standard. Two users: the ACPI repair identifies the stock
 * table it repairs by a digest, and the Secure Boot module computes the Authenticode digest of an image and the
 * digests under an RSA signature. No code is copied from an implementation.
 */
#ifndef EFL_SHA256_H
#define EFL_SHA256_H
#include <stddef.h>

#define EFL_SHA256_BYTES 32u

struct efl_sha256 {
	unsigned state[8];
	unsigned long long bytes;	/* the message length so far */
	unsigned char block[64];
};

void efl_sha256_init(struct efl_sha256 *s);
void efl_sha256_update(struct efl_sha256 *s, const void *data, size_t bytes);
void efl_sha256_final(struct efl_sha256 *s, unsigned char digest[EFL_SHA256_BYTES]);
void efl_sha256(const void *data, size_t bytes, unsigned char digest[EFL_SHA256_BYTES]);

#endif
