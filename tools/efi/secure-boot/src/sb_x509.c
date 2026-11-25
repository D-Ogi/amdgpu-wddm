/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The certificate reader (sb_x509.h). The structure names in the comments are those of RFC 5280 section 4.1. */
#include <stddef.h>
#include <string.h>
#include "efl_sha256.h"
#include "sb_x509.h"

const unsigned char sb_oid_rsa_encryption[9] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };	/* 1.2.840.113549.1.1.1 */
const unsigned char sb_oid_sha256_rsa[9] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };	/* 1.2.840.113549.1.1.11 */
const unsigned char sb_oid_sha256[9] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };		/* 2.16.840.1.101.3.4.2.1 */
static const unsigned char oid_basic_constraints[3] = { 0x55, 0x1D, 0x13 };				/* 2.5.29.19 */

/* AlgorithmIdentifier ::= SEQUENCE { algorithm OBJECT IDENTIFIER, parameters ANY OPTIONAL }. Sets *oid. */
static int algorithm(struct sb_span *in, struct sb_span *oid)
{
	struct sb_tlv seq, id;
	if (!sb_der_expect(in, SB_DER_SEQUENCE, &seq) || !sb_der_expect(&seq.value, SB_DER_OID, &id)) return 0;
	*oid = id.value;
	return 1;
}

/* SubjectPublicKeyInfo with an RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }. */
static void public_key(struct sb_span spki, struct sb_cert *out)
{
	struct sb_tlv bits, seq, modulus, exponent;
	struct sb_span oid, key;
	if (!algorithm(&spki, &oid) || !sb_span_is(&oid, sb_oid_rsa_encryption, sizeof(sb_oid_rsa_encryption))) return;
	if (!sb_der_expect(&spki, SB_DER_BIT_STRING, &bits) || bits.value.n < 1u || bits.value.p[0] != 0) return;
	key.p = bits.value.p + 1;
	key.n = bits.value.n - 1u;
	if (!sb_der_expect(&key, SB_DER_SEQUENCE, &seq) || key.n != 0) return;
	if (!sb_der_expect(&seq.value, SB_DER_INTEGER, &modulus) || !sb_der_expect(&seq.value, SB_DER_INTEGER, &exponent))
		return;
	/* A negative INTEGER is not a modulus. */
	if (modulus.value.n == 0 || (modulus.value.p[0] & 0x80u) || exponent.value.n == 0) return;
	out->key.modulus = modulus.value.p;
	out->key.modulus_bytes = modulus.value.n;
	out->key.exponent = exponent.value.p;
	out->key.exponent_bytes = exponent.value.n;
	out->rsa = 1;
}

/* Extensions ::= SEQUENCE OF Extension { extnID, critical BOOLEAN DEFAULT FALSE, extnValue OCTET STRING }. */
static void extensions(struct sb_span list, struct sb_cert *out)
{
	struct sb_tlv extension, id, value, constraints, flag;
	while (list.n) {
		struct sb_span e;
		if (!sb_der_expect(&list, SB_DER_SEQUENCE, &extension)) return;
		e = extension.value;
		if (!sb_der_expect(&e, SB_DER_OID, &id)) return;
		(void)sb_der_expect(&e, SB_DER_BOOLEAN, &flag);
		if (!sb_der_expect(&e, SB_DER_OCTET_STRING, &value)) return;
		if (!sb_span_is(&id.value, oid_basic_constraints, sizeof(oid_basic_constraints))) continue;
		/* BasicConstraints ::= SEQUENCE { cA BOOLEAN DEFAULT FALSE, pathLenConstraint INTEGER OPTIONAL } */
		if (!sb_der_expect(&value.value, SB_DER_SEQUENCE, &constraints)) return;
		if (sb_der_expect(&constraints.value, SB_DER_BOOLEAN, &flag) && flag.value.n == 1u && flag.value.p[0] != 0)
			out->ca = 1;
		return;
	}
}

int sb_cert_parse(const unsigned char *der, size_t bytes, struct sb_cert *out)
{
	struct sb_span in, body, tbs, oid;
	struct sb_tlv certificate, tbs_tlv, t, signature;
	memset(out, 0, sizeof(*out));
	in.p = der;
	in.n = bytes;
	/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue BIT STRING } */
	if (!sb_der_expect(&in, SB_DER_SEQUENCE, &certificate) || in.n != 0) return 0;
	out->whole = certificate.whole;
	body = certificate.value;
	if (!sb_der_expect(&body, SB_DER_SEQUENCE, &tbs_tlv)) return 0;
	out->tbs = tbs_tlv.whole;
	if (!algorithm(&body, &oid)) return 0;
	out->sha256_rsa = sb_span_is(&oid, sb_oid_sha256_rsa, sizeof(sb_oid_sha256_rsa));
	if (!sb_der_expect(&body, SB_DER_BIT_STRING, &signature) || signature.value.n < 1u || signature.value.p[0] != 0 ||
	    body.n != 0)
		return 0;
	out->signature.p = signature.value.p + 1;
	out->signature.n = signature.value.n - 1u;

	/* TBSCertificate ::= SEQUENCE { [0] version OPTIONAL, serialNumber, signature, issuer, validity, subject,
	 * subjectPublicKeyInfo, [1] and [2] unique identifiers OPTIONAL, [3] extensions OPTIONAL } */
	tbs = tbs_tlv.value;
	(void)sb_der_expect(&tbs, SB_DER_CONTEXT(0), &t);
	if (!sb_der_expect(&tbs, SB_DER_INTEGER, &t)) return 0;
	out->serial = t.value;
	if (!algorithm(&tbs, &oid)) return 0;
	if (!sb_der_expect(&tbs, SB_DER_SEQUENCE, &t)) return 0;
	out->issuer = t.whole;
	if (!sb_der_expect(&tbs, SB_DER_SEQUENCE, &t)) return 0;	/* validity: not checked, see sb_x509.h */
	if (!sb_der_expect(&tbs, SB_DER_SEQUENCE, &t)) return 0;
	out->subject = t.whole;
	if (!sb_der_expect(&tbs, SB_DER_SEQUENCE, &t)) return 0;
	public_key(t.value, out);
	while (tbs.n) {
		if (!sb_der_next(&tbs, &t)) return 0;
		if (t.tag == SB_DER_CONTEXT(3)) {
			struct sb_tlv list;
			if (sb_der_expect(&t.value, SB_DER_SEQUENCE, &list)) extensions(list.value, out);
		}
	}
	return 1;
}

int sb_cert_signed_by(const struct sb_cert *cert, const struct sb_cert *issuer)
{
	unsigned char digest[EFL_SHA256_BYTES];
	if (!cert->sha256_rsa || !issuer->rsa || !sb_span_equal(&cert->issuer, &issuer->subject)) return 0;
	efl_sha256(cert->tbs.p, cert->tbs.n, digest);
	return sb_rsa_verify_sha256(&issuer->key, cert->signature.p, cert->signature.n, digest);
}
