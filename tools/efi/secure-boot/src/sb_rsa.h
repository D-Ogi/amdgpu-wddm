/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * RSA signature verification, RSASSA-PKCS1-v1_5 with SHA-256 (RFC 8017, section 8.2.2, with the EMSA-PKCS1-v1_5
 * encoding of section 9.2). Public key operation only: this code holds no private key and has no signing
 * function. Written from the RFC. No code is copied from an implementation.
 *
 * Limits: a modulus of 2048 to 4096 bits, a public exponent of at most 32 bits.
 */
#ifndef SB_RSA_H
#define SB_RSA_H
#include <stddef.h>

#define SB_RSA_MIN_BYTES	256u
#define SB_RSA_MAX_BYTES	512u

struct sb_rsa_key {
	const unsigned char *modulus;	/* big endian, as in the certificate. Leading zero octets are allowed */
	size_t modulus_bytes;
	const unsigned char *exponent;	/* big endian */
	size_t exponent_bytes;
};

/* 1 when signature is a valid signature of the SHA-256 digest under the key. */
int sb_rsa_verify_sha256(const struct sb_rsa_key *key, const unsigned char *signature, size_t signature_bytes,
			 const unsigned char digest[32]);

#endif
