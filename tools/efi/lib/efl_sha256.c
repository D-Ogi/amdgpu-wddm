/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* SHA-256 (efl_sha256.h). The constants are those of FIPS 180-4 section 4.2.2, the functions those of section
 * 4.1.2, and the padding that of section 5.1.1. */
#include <stddef.h>
#include <string.h>
#include "efl_sha256.h"

static const unsigned k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static unsigned rotr(unsigned x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void compress(struct efl_sha256 *s, const unsigned char *p)
{
	unsigned w[64], a, b, c, d, e, f, g, h, i;
	for (i = 0; i < 16u; i++)
		w[i] = ((unsigned)p[4u * i] << 24) | ((unsigned)p[4u * i + 1u] << 16) | ((unsigned)p[4u * i + 2u] << 8) |
		       (unsigned)p[4u * i + 3u];
	for (i = 16u; i < 64u; i++) {
		unsigned s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
		unsigned s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
		w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
	}
	a = s->state[0]; b = s->state[1]; c = s->state[2]; d = s->state[3];
	e = s->state[4]; f = s->state[5]; g = s->state[6]; h = s->state[7];
	for (i = 0; i < 64u; i++) {
		unsigned t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
		unsigned t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
	s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

void efl_sha256_init(struct efl_sha256 *s)
{
	s->state[0] = 0x6a09e667u; s->state[1] = 0xbb67ae85u; s->state[2] = 0x3c6ef372u; s->state[3] = 0xa54ff53au;
	s->state[4] = 0x510e527fu; s->state[5] = 0x9b05688cu; s->state[6] = 0x1f83d9abu; s->state[7] = 0x5be0cd19u;
	s->bytes = 0;
}

void efl_sha256_update(struct efl_sha256 *s, const void *data, size_t bytes)
{
	const unsigned char *p = data;
	size_t have = (size_t)(s->bytes & 63u);
	s->bytes += bytes;
	if (have) {
		size_t take = 64u - have;
		if (take > bytes) take = bytes;
		memcpy(s->block + have, p, take);
		p += take;
		bytes -= take;
		if (have + take < 64u) return;
		compress(s, s->block);
	}
	for (; bytes >= 64u; p += 64, bytes -= 64u) compress(s, p);
	if (bytes) memcpy(s->block, p, bytes);
}

void efl_sha256_final(struct efl_sha256 *s, unsigned char digest[EFL_SHA256_BYTES])
{
	unsigned long long bits = s->bytes * 8u;
	size_t have = (size_t)(s->bytes & 63u);
	unsigned i;
	s->block[have++] = 0x80u;
	if (have > 56u) {
		memset(s->block + have, 0, 64u - have);
		compress(s, s->block);
		have = 0;
	}
	memset(s->block + have, 0, 56u - have);
	for (i = 0; i < 8u; i++) s->block[56u + i] = (unsigned char)(bits >> (56u - 8u * i));
	compress(s, s->block);
	for (i = 0; i < 8u; i++) {
		digest[4u * i] = (unsigned char)(s->state[i] >> 24);
		digest[4u * i + 1u] = (unsigned char)(s->state[i] >> 16);
		digest[4u * i + 2u] = (unsigned char)(s->state[i] >> 8);
		digest[4u * i + 3u] = (unsigned char)s->state[i];
	}
}

void efl_sha256(const void *data, size_t bytes, unsigned char digest[EFL_SHA256_BYTES])
{
	struct efl_sha256 s;
	efl_sha256_init(&s);
	efl_sha256_update(&s, data, bytes);
	efl_sha256_final(&s, digest);
}
