/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The parts of an X.509 certificate (RFC 5280, section 4.1) that the image check needs: the signed part, the
 * names, the serial number, an RSA public key, the signature, and the cA flag of the basic constraints (section
 * 4.2.1.9). Only RSA keys and the signature algorithm sha256WithRSAEncryption are understood.
 *
 * What this code does not check, on purpose, as the firmware of a PC does not check it for boot images either:
 * the validity dates (a boot manager signed years ago must still start, and the stage has no trusted clock), the
 * key usage and the extended key usage. Revocation is the forbidden database (dbx) only.
 */
#ifndef SB_X509_H
#define SB_X509_H
#include <stddef.h>
#include "sb_der.h"
#include "sb_rsa.h"

struct sb_cert {
	struct sb_span whole;		/* the Certificate */
	struct sb_span tbs;		/* the TBSCertificate with its header: the bytes under the signature */
	struct sb_span serial;		/* the contents of the serial number */
	struct sb_span issuer, subject;	/* each Name with its header */
	int rsa;			/* the public key is an RSA key */
	struct sb_rsa_key key;
	int sha256_rsa;			/* the signature algorithm is sha256WithRSAEncryption */
	struct sb_span signature;
	int ca;				/* basicConstraints is present with cA TRUE */
};

/* 1 when the bytes are one certificate and nothing else. */
int sb_cert_parse(const unsigned char *der, size_t bytes, struct sb_cert *out);

/* 1 when the subject of issuer is the issuer of cert and the signature of cert verifies under the key of issuer. */
int sb_cert_signed_by(const struct sb_cert *cert, const struct sb_cert *issuer);

/* The object identifiers that the certificate and the signature readers share: the contents octets. */
extern const unsigned char sb_oid_rsa_encryption[9];
extern const unsigned char sb_oid_sha256_rsa[9];
extern const unsigned char sb_oid_sha256[9];

#endif
