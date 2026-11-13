/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The verdict on one PE image against an allowed database (db) and a forbidden database (dbx). The order of the
 * checks is that of the UEFI Specification for image authorization, compared with DxeImageVerificationHandler in
 * edk2 SecurityPkg/Library/DxeImageVerificationLib (commit 8628578dacd3). No code is copied:
 *
 * 1. The Authenticode digest of the image in dbx: forbidden.
 * 2. For each signature whose digest is that of the image and whose signer signed it: walk from the signer's
 *    certificate up through the certificates of the signature, to the top. A certificate on the way that is in
 *    dbx, or that a dbx certificate signed: forbidden. Else a certificate on the way that is in db, or that a db
 *    certificate signed: allowed by signature.
 * 3. The digest of the image in db: allowed by digest.
 * 4. Otherwise not allowed.
 *
 * What it does not do is in sb_x509.h (no dates, no key usage) and in sb_siglist.h (two entry types).
 */
#ifndef SB_VERIFY_H
#define SB_VERIFY_H
#include <stddef.h>

struct sb_databases {
	const unsigned char *db, *dbx;	/* EFI_SIGNATURE_LIST data, NULL with 0 bytes when there is none */
	size_t db_bytes, dbx_bytes;
};

enum sb_verdict {
	SB_ALLOWED_SIGNATURE = 0,	/* a signature chains to a db certificate */
	SB_ALLOWED_DIGEST,		/* the digest of the image is in db */
	SB_FORBIDDEN_DIGEST,		/* the digest of the image is in dbx */
	SB_FORBIDDEN_CERTIFICATE,	/* a certificate of a valid signature is in dbx */
	SB_NOT_SIGNED,			/* no signature of the supported kind, and the digest is not in db */
	SB_NOT_TRUSTED,			/* a valid signature, and no certificate of it leads to db */
	SB_BAD_SIGNATURE,		/* each signature is for other bytes, or does not verify */
	SB_NOT_AN_IMAGE			/* not a sound PE image */
};

struct sb_result {
	enum sb_verdict verdict;
	unsigned char digest[32];	/* the Authenticode SHA-256 digest, zero for SB_NOT_AN_IMAGE */
};

void sb_verify_image(const unsigned char *image, size_t bytes, const struct sb_databases *databases,
		     struct sb_result *out);

/* 1 for the two allowed verdicts. */
int sb_verdict_allows(enum sb_verdict verdict);
const char *sb_verdict_text(enum sb_verdict verdict);

#endif
