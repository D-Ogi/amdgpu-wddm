/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The Authenticode signature reader (sb_pkcs7.h). The structure names in the comments are those of RFC 2315. */
#include <stddef.h>
#include <string.h>
#include "efl_sha256.h"
#include "sb_pkcs7.h"

static const unsigned char oid_signed_data[9] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };	/* 1.2.840.113549.1.7.2 */
static const unsigned char oid_content_type[9] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x03 };	/* 1.2.840.113549.1.9.3 */
static const unsigned char oid_message_digest[9] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04 };	/* 1.2.840.113549.1.9.4 */
/* SPC_INDIRECT_DATA_OBJID, 1.3.6.1.4.1.311.2.1.4 */
static const unsigned char oid_spc_indirect_data[10] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 };

/* AlgorithmIdentifier: sets *oid. */
static int algorithm(struct sb_span *in, struct sb_span *oid)
{
	struct sb_tlv seq, id;
	if (!sb_der_expect(in, SB_DER_SEQUENCE, &seq) || !sb_der_expect(&seq.value, SB_DER_OID, &id)) return 0;
	*oid = id.value;
	return 1;
}

static int is_sha256(const struct sb_span *oid) { return sb_span_is(oid, sb_oid_sha256, sizeof(sb_oid_sha256)); }

/* SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue, messageDigest DigestInfo },
 * DigestInfo ::= SEQUENCE { digestAlgorithm AlgorithmIdentifier, digest OCTET STRING }. */
static int indirect_data(struct sb_span content, struct sb_pkcs7 *out)
{
	struct sb_tlv data, info, digest;
	struct sb_span oid;
	if (!sb_der_expect(&content, SB_DER_SEQUENCE, &data) || !sb_der_expect(&content, SB_DER_SEQUENCE, &info) ||
	    content.n != 0)
		return 0;
	if (!algorithm(&info.value, &oid) || !is_sha256(&oid)) return 0;
	if (!sb_der_expect(&info.value, SB_DER_OCTET_STRING, &digest) || digest.value.n != EFL_SHA256_BYTES) return 0;
	out->image_digest = digest.value;
	return 1;
}

/* The authenticated attributes: SET OF Attribute { type OID, values SET OF ANY }. The two that PKCS #7 requires
 * must be there once: the content type, which must name the SpcIndirectDataContent, and the message digest. */
static int attributes(struct sb_span list, struct sb_pkcs7 *out)
{
	int content_type = 0, message_digest = 0;
	while (list.n) {
		struct sb_tlv attribute, type, values, value;
		if (!sb_der_expect(&list, SB_DER_SEQUENCE, &attribute)) return 0;
		if (!sb_der_expect(&attribute.value, SB_DER_OID, &type) ||
		    !sb_der_expect(&attribute.value, SB_DER_SET, &values))
			return 0;
		if (sb_span_is(&type.value, oid_content_type, sizeof(oid_content_type))) {
			if (content_type++ || !sb_der_expect(&values.value, SB_DER_OID, &value) || values.value.n != 0 ||
			    !sb_span_is(&value.value, oid_spc_indirect_data, sizeof(oid_spc_indirect_data)))
				return 0;
		} else if (sb_span_is(&type.value, oid_message_digest, sizeof(oid_message_digest))) {
			if (message_digest++ || !sb_der_expect(&values.value, SB_DER_OCTET_STRING, &value) ||
			    values.value.n != 0 || value.value.n != EFL_SHA256_BYTES)
				return 0;
			out->content_digest = value.value;
		}
	}
	return content_type == 1 && message_digest == 1;
}

/* SignerInfo ::= SEQUENCE { version, issuerAndSerialNumber, digestAlgorithm, [0] authenticatedAttributes
 * OPTIONAL, digestEncryptionAlgorithm, encryptedDigest OCTET STRING, [1] unauthenticatedAttributes OPTIONAL }.
 * Authenticode always has the authenticated attributes, so a SignerInfo without them is refused. */
static int signer_info(struct sb_span info, struct sb_pkcs7 *out)
{
	struct sb_tlv t, name, serial, attrs, digest;
	struct sb_span oid;
	if (!sb_der_expect(&info, SB_DER_INTEGER, &t)) return 0;
	if (!sb_der_expect(&info, SB_DER_SEQUENCE, &t)) return 0;
	if (!sb_der_expect(&t.value, SB_DER_SEQUENCE, &name) || !sb_der_expect(&t.value, SB_DER_INTEGER, &serial)) return 0;
	out->issuer = name.whole;
	out->serial = serial.value;
	if (!algorithm(&info, &oid) || !is_sha256(&oid)) return 0;
	if (!sb_der_expect(&info, SB_DER_CONTEXT(0), &attrs) || !attributes(attrs.value, out)) return 0;
	out->attributes = attrs.whole;
	/* Signers name the RSA step as rsaEncryption or as sha256WithRSAEncryption. The two mean the same here. */
	if (!algorithm(&info, &oid) || (!sb_span_is(&oid, sb_oid_rsa_encryption, sizeof(sb_oid_rsa_encryption)) &&
					!sb_span_is(&oid, sb_oid_sha256_rsa, sizeof(sb_oid_sha256_rsa))))
		return 0;
	if (!sb_der_expect(&info, SB_DER_OCTET_STRING, &digest)) return 0;
	out->signature = digest.value;
	return 1;
}

int sb_pkcs7_parse(const unsigned char *der, size_t bytes, struct sb_pkcs7 *out)
{
	struct sb_span in, body, list;
	struct sb_tlv outer, type, wrapped, signed_data, t, content_info, content, signers, signer;
	memset(out, 0, sizeof(*out));
	in.p = der;
	in.n = bytes;
	/* ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT content }. Bytes after it are the padding of the
	 * certificate table, so they are not an error. */
	if (!sb_der_expect(&in, SB_DER_SEQUENCE, &outer)) return 0;
	body = outer.value;
	if (!sb_der_expect(&body, SB_DER_OID, &type) || !sb_span_is(&type.value, oid_signed_data, sizeof(oid_signed_data)))
		return 0;
	if (!sb_der_expect(&body, SB_DER_CONTEXT(0), &wrapped) || !sb_der_expect(&wrapped.value, SB_DER_SEQUENCE, &signed_data))
		return 0;
	/* SignedData ::= SEQUENCE { version, digestAlgorithms SET, contentInfo, [0] certificates OPTIONAL,
	 * [1] crls OPTIONAL, signerInfos SET } */
	body = signed_data.value;
	if (!sb_der_expect(&body, SB_DER_INTEGER, &t) || !sb_der_expect(&body, SB_DER_SET, &t)) return 0;
	if (!sb_der_expect(&body, SB_DER_SEQUENCE, &content_info)) return 0;
	list = content_info.value;
	if (!sb_der_expect(&list, SB_DER_OID, &type) ||
	    !sb_span_is(&type.value, oid_spc_indirect_data, sizeof(oid_spc_indirect_data)))
		return 0;
	if (!sb_der_expect(&list, SB_DER_CONTEXT(0), &wrapped) || !sb_der_expect(&wrapped.value, SB_DER_SEQUENCE, &content))
		return 0;
	/* The message digest of PKCS #7 covers the contents octets of the content, without its identifier and length
	 * (RFC 2315 section 9.3). */
	out->content = content.value;
	if (!indirect_data(content.value, out)) return 0;
	if (sb_der_expect(&body, SB_DER_CONTEXT(0), &t)) out->certificates = t.value;
	(void)sb_der_expect(&body, SB_DER_CONTEXT(1), &t);
	if (!sb_der_expect(&body, SB_DER_SET, &signers)) return 0;
	list = signers.value;
	if (!sb_der_expect(&list, SB_DER_SEQUENCE, &signer) || list.n != 0) return 0;
	return signer_info(signer.value, out);
}

int sb_pkcs7_next_cert(struct sb_span *cursor, struct sb_cert *out)
{
	struct sb_tlv t;
	if (!sb_der_expect(cursor, SB_DER_SEQUENCE, &t)) return 0;
	return sb_cert_parse(t.whole.p, t.whole.n, out);
}

int sb_pkcs7_signer(const struct sb_pkcs7 *p7, struct sb_cert *out)
{
	struct sb_span cursor = p7->certificates;
	while (sb_pkcs7_next_cert(&cursor, out))
		if (sb_span_equal(&out->issuer, &p7->issuer) && sb_span_equal(&out->serial, &p7->serial)) return 1;
	return 0;
}

int sb_pkcs7_signed_by(const struct sb_pkcs7 *p7, const struct sb_cert *cert)
{
	static const unsigned char set_tag = SB_DER_SET;
	unsigned char digest[EFL_SHA256_BYTES];
	struct efl_sha256 hash;
	if (!cert->rsa || p7->attributes.n < 2u) return 0;
	efl_sha256(p7->content.p, p7->content.n, digest);
	if (memcmp(digest, p7->content_digest.p, EFL_SHA256_BYTES) != 0) return 0;
	/* The signature covers the attributes as a SET OF: the [0] identifier octet is replaced by the SET one
	 * (RFC 2315 section 9.3). */
	efl_sha256_init(&hash);
	efl_sha256_update(&hash, &set_tag, 1u);
	efl_sha256_update(&hash, p7->attributes.p + 1, p7->attributes.n - 1u);
	efl_sha256_final(&hash, digest);
	return sb_rsa_verify_sha256(&cert->key, p7->signature.p, p7->signature.n, digest);
}
