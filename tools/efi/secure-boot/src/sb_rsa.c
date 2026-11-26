/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * RSA signature verification (sb_rsa.h). The modular exponentiation is a square-and-multiply over Montgomery
 * multiplication with 32-bit limbs. The values here are public (a signature, a public key), so the code does not
 * need a constant running time.
 */
#include <stddef.h>
#include <string.h>
#include "sb_rsa.h"

#define LIMBS		(SB_RSA_MAX_BYTES / 4u)
#define DIGEST_BYTES	32u

typedef unsigned int u32;
typedef unsigned long long u64;

/* The DigestInfo of SHA-256 before the digest (RFC 8017 section 9.2, note 1). */
static const unsigned char sha256_prefix[19] = {
	0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

static void from_be(u32 *out, unsigned limbs, const unsigned char *in, size_t bytes)
{
	size_t i;
	memset(out, 0, limbs * sizeof(u32));
	for (i = 0; i < bytes; i++) out[i / 4u] |= (u32)in[bytes - 1u - i] << (8u * (i % 4u));
}

static int greater_equal(const u32 *a, const u32 *b, unsigned n)
{
	unsigned i;
	for (i = n; i-- > 0;)
		if (a[i] != b[i]) return a[i] > b[i];
	return 1;
}

static void subtract(u32 *a, const u32 *b, unsigned n)
{
	u64 borrow = 0;
	unsigned i;
	for (i = 0; i < n; i++) {
		u64 d = (u64)a[i] - b[i] - borrow;
		a[i] = (u32)d;
		borrow = (d >> 32) & 1u;
	}
}

/* out = a * b / 2^(32 n) mod m, for a and b below m (the coarsely integrated operand scanning form). */
static void montgomery(u32 *out, const u32 *a, const u32 *b, const u32 *m, u32 m0_inverse, unsigned n)
{
	u32 t[LIMBS + 2u];
	unsigned i, j;
	memset(t, 0, (n + 2u) * sizeof(u32));
	for (i = 0; i < n; i++) {
		u64 carry = 0, uv;
		u32 q;
		for (j = 0; j < n; j++) {
			uv = (u64)a[j] * b[i] + t[j] + carry;
			t[j] = (u32)uv;
			carry = uv >> 32;
		}
		uv = (u64)t[n] + carry;
		t[n] = (u32)uv;
		t[n + 1u] = (u32)(uv >> 32);
		q = t[0] * m0_inverse;
		uv = (u64)q * m[0] + t[0];
		carry = uv >> 32;
		for (j = 1; j < n; j++) {
			uv = (u64)q * m[j] + t[j] + carry;
			t[j - 1u] = (u32)uv;
			carry = uv >> 32;
		}
		uv = (u64)t[n] + carry;
		t[n - 1u] = (u32)uv;
		t[n] = t[n + 1u] + (u32)(uv >> 32);
	}
	if (t[n] || greater_equal(t, m, n)) subtract(t, m, n);
	memcpy(out, t, n * sizeof(u32));
}

/* r = 2^(32 n doublings / 32) mod m: doubles r, which starts at 1, the given number of times. */
static void power_of_two(u32 *r, const u32 *m, unsigned n, unsigned doublings)
{
	unsigned i, j;
	memset(r, 0, n * sizeof(u32));
	r[0] = 1;
	for (i = 0; i < doublings; i++) {
		u32 carry = 0;
		for (j = 0; j < n; j++) {
			u32 next = r[j] >> 31;
			r[j] = (r[j] << 1) | carry;
			carry = next;
		}
		if (carry || greater_equal(r, m, n)) subtract(r, m, n);
	}
}

int sb_rsa_verify_sha256(const struct sb_rsa_key *key, const unsigned char *signature, size_t signature_bytes,
			 const unsigned char digest[32])
{
	u32 m[LIMBS], s[LIMBS], rr[LIMBS], one[LIMBS], base[LIMBS], acc[LIMBS];
	unsigned char em[SB_RSA_MAX_BYTES], want[SB_RSA_MAX_BYTES];
	const unsigned char *modulus = key->modulus, *exponent = key->exponent;
	size_t k = key->modulus_bytes, e_bytes = key->exponent_bytes, i, pad;
	u32 e = 0, m0_inverse, x;
	unsigned n;
	int bit;

	if (modulus == NULL || exponent == NULL || signature == NULL) return 0;
	while (k > 0 && modulus[0] == 0) { modulus++; k--; }
	while (e_bytes > 0 && exponent[0] == 0) { exponent++; e_bytes--; }
	if (k < SB_RSA_MIN_BYTES || k > SB_RSA_MAX_BYTES || !(modulus[k - 1u] & 1u)) return 0;
	if (e_bytes == 0 || e_bytes > 4u || signature_bytes != k) return 0;
	for (i = 0; i < e_bytes; i++) e = (e << 8) | exponent[i];
	if (e < 3u || !(e & 1u)) return 0;

	n = (unsigned)((k + 3u) / 4u);
	from_be(m, n, modulus, k);
	from_be(s, n, signature, k);
	if (greater_equal(s, m, n)) return 0;		/* the signature representative must be below the modulus */

	/* The inverse of m[0] modulo 2^32 by Newton's iteration: each step doubles the correct low bits. */
	x = m[0];
	for (i = 0; i < 5u; i++) x *= 2u - m[0] * x;
	m0_inverse = 0u - x;

	power_of_two(rr, m, n, 64u * n);		/* R^2 mod m, with R = 2^(32 n) */
	memset(one, 0, n * sizeof(u32));
	one[0] = 1;
	montgomery(acc, rr, one, m, m0_inverse, n);	/* R mod m: the Montgomery form of 1 */
	montgomery(base, s, rr, m, m0_inverse, n);	/* the Montgomery form of the signature */
	for (bit = 31; bit >= 0; bit--) {
		montgomery(acc, acc, acc, m, m0_inverse, n);
		if ((e >> bit) & 1u) montgomery(acc, acc, base, m, m0_inverse, n);
	}
	montgomery(acc, acc, one, m, m0_inverse, n);	/* out of the Montgomery form */
	for (i = 0; i < k; i++) em[k - 1u - i] = (unsigned char)(acc[i / 4u] >> (8u * (i % 4u)));

	/* EM = 0x00 0x01 PS 0x00 T, with PS of 0xFF octets and T the DigestInfo. The whole block is compared, so no
	 * byte of it is parsed. */
	pad = k - 3u - sizeof(sha256_prefix) - DIGEST_BYTES;
	want[0] = 0x00;
	want[1] = 0x01;
	memset(want + 2, 0xFF, pad);
	want[2u + pad] = 0x00;
	memcpy(want + 3u + pad, sha256_prefix, sizeof(sha256_prefix));
	memcpy(want + 3u + pad + sizeof(sha256_prefix), digest, DIGEST_BYTES);
	return !memcmp(em, want, k);
}
