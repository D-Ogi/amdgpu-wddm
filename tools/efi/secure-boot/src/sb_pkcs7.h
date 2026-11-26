/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Authenticode signature of a PE image: a PKCS #7 SignedData (RFC 2315, sections 7 and 9) whose content is an
 * SpcIndirectDataContent ("Windows Authenticode Portable Executable Signature Format", version 1.0, 2008). Only
 * the SHA-256 digest and RSA signatures are understood. A signature with another algorithm does not parse, and
 * the image is then judged as if that signature were not there.
 *
 * Authenticode allows exactly one signer for each SignedData. A SignedData with another count is refused.
 * Countersignatures and time stamps (the unauthenticated attributes) are not read.
 */
#ifndef SB_PKCS7_H
#define SB_PKCS7_H
#include <stddef.h>
#include "sb_der.h"
#include "sb_x509.h"

struct sb_pkcs7 {
	struct sb_span image_digest;	/* the SHA-256 digest of the image that the signature names: 32 bytes */
	struct sb_span content;		/* the contents octets of the SpcIndirectDataContent */
	struct sb_span certificates;	/* the certificates of the SignedData, one after the other (can be empty) */
	struct sb_span issuer, serial;	/* which certificate is the signer's */
	struct sb_span attributes;	/* the authenticated attributes with their [0] header */
	struct sb_span content_digest;	/* the messageDigest attribute: 32 bytes */
	struct sb_span signature;	/* the encrypted digest */
};

/* 1 when the bytes are one Authenticode SignedData of the supported kind. */
int sb_pkcs7_parse(const unsigned char *der, size_t bytes, struct sb_pkcs7 *out);

/* The next certificate of the SignedData. *cursor starts as p7->certificates. 1 with a certificate, 0 at the end
 * or at an element that is not a certificate this code reads. */
int sb_pkcs7_next_cert(struct sb_span *cursor, struct sb_cert *out);

/* The certificate of the signer: the one whose issuer and serial number the SignerInfo names. 1 when found. */
int sb_pkcs7_signer(const struct sb_pkcs7 *p7, struct sb_cert *out);

/* 1 when the signature holds under the key of cert: the messageDigest attribute is the digest of the content, and
 * the encrypted digest is an RSA signature of the authenticated attributes. */
int sb_pkcs7_signed_by(const struct sb_pkcs7 *p7, const struct sb_cert *cert);

#endif
